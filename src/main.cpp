#include "notification.h"

#include <QBackingStore>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSurfaceFormat>
#include <QSocketNotifier>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <LayerShellQt/Window>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <systemd/sd-bus.h>
#include <unistd.h>

struct AppConfig
{
    QString mode = QStringLiteral("secondary");
    QStringList screens;
    QString position = QStringLiteral("bottom-right");
    int lifetimeMs = 5000;
    int width = 332;
    int maxNotifications = 5;
    int backgroundOpacity = 100;
    int leftMargin = 18;
    int rightMargin = 18;
    int topMargin = 18;
    int bottomMargin = 58;
    int gap = 12;
};

static AppConfig loadConfig()
{
    AppConfig config;

    QString configPath =
        qEnvironmentVariable("PLASMA_NOTIFICATION_MIRROR_CONFIG").trimmed();

    if (configPath.isEmpty()) {
        const QString configDir =
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + QStringLiteral("/plasma-notification-mirror");
        configPath = configDir + QStringLiteral("/config.ini");
    } else {
        qInfo() << "Using config override:" << configPath;
    }

    if (!QFileInfo::exists(configPath)) {
        qInfo() << "No config file found; using zero-configuration defaults:" << configPath;
        return config;
    }

    QSettings settings(configPath, QSettings::IniFormat);

    config.mode = settings.value(QStringLiteral("mode"), config.mode).toString().trimmed().toLower();
    config.screens = settings.value(QStringLiteral("screens"), QString()).toString().split(',', Qt::SkipEmptyParts);
    for (QString &screen : config.screens)
        screen = screen.trimmed();

    config.position =
        settings.value(QStringLiteral("position"), config.position)
            .toString()
            .trimmed()
            .toLower();

    const QStringList validPositions = {
        QStringLiteral("top-left"),
        QStringLiteral("top-right"),
        QStringLiteral("bottom-left"),
        QStringLiteral("bottom-right")
    };

    if (!validPositions.contains(config.position)) {
        qWarning() << "Unknown position" << config.position
                   << "; using 'bottom-right'.";
        config.position = QStringLiteral("bottom-right");
    }

    config.lifetimeMs = qBound(500, settings.value(QStringLiteral("lifetime_ms"), config.lifetimeMs).toInt(), 60000);
    config.width = qBound(220, settings.value(QStringLiteral("width"), config.width).toInt(), 900);
    config.maxNotifications = qBound(1, settings.value(QStringLiteral("max_notifications"), config.maxNotifications).toInt(), 20);
    config.backgroundOpacity =
        qBound(0,
               settings.value(QStringLiteral("background_opacity"),
                              config.backgroundOpacity).toInt(),
               100);

    config.leftMargin =
        qMax(0, settings.value(QStringLiteral("left_margin"), config.leftMargin).toInt());
    config.rightMargin =
        qMax(0, settings.value(QStringLiteral("right_margin"), config.rightMargin).toInt());
    config.topMargin =
        qMax(0, settings.value(QStringLiteral("top_margin"), config.topMargin).toInt());
    config.bottomMargin =
        qMax(0, settings.value(QStringLiteral("bottom_margin"), config.bottomMargin).toInt());

    config.gap = qBound(0, settings.value(QStringLiteral("gap"), config.gap).toInt(), 100);

    qInfo() << "Popup position:" << config.position;
    qInfo() << "Margins:"
            << "left=" << config.leftMargin
            << "right=" << config.rightMargin
            << "top=" << config.topMargin
            << "bottom=" << config.bottomMargin;

    return config;
}

static QList<QScreen *> selectTargetScreens(const AppConfig &config)
{
    const QList<QScreen *> allScreens = QGuiApplication::screens();
    QScreen *primary = QGuiApplication::primaryScreen();
    QList<QScreen *> targets;

    auto addFirstSecondary = [&]() {
        for (QScreen *screen : allScreens) {
            if (screen != primary) {
                targets.append(screen);
                return;
            }
        }
    };

    if (config.mode == QStringLiteral("primary")) {
        if (primary)
            targets.append(primary);
    } else if (config.mode == QStringLiteral("all")) {
        targets = allScreens;
    } else if (config.mode == QStringLiteral("all-secondary")) {
        for (QScreen *screen : allScreens) {
            if (screen != primary)
                targets.append(screen);
        }
    } else if (config.mode == QStringLiteral("screens")) {
        for (const QString &wanted : config.screens) {
            auto it = std::find_if(allScreens.cbegin(), allScreens.cend(), [&](QScreen *screen) {
                return screen->name() == wanted;
            });
            if (it != allScreens.cend())
                targets.append(*it);
            else
                qWarning() << "Configured monitor was not found:" << wanted;
        }

        if (targets.isEmpty()) {
            qWarning() << "No configured monitors were available; falling back to the first secondary monitor.";
            addFirstSecondary();
        }
    } else {
        if (config.mode != QStringLiteral("secondary"))
            qWarning() << "Unknown mode" << config.mode << "; using 'secondary'.";
        addFirstSecondary();
    }

    QList<QScreen *> uniqueTargets;
    for (QScreen *screen : targets) {
        if (!uniqueTargets.contains(screen))
            uniqueTargets.append(screen);
    }
    return uniqueTargets;
}

static int calculatePopupHeight(const NotificationData &data, int popupWidth)
{
    constexpr int minHeight = 82;
    constexpr int leftPadding = 14;
    constexpr int rightPadding = 14;
    constexpr int titleTop = 34;
    constexpr int titleHeight = 20;
    constexpr int bodyTop = 55;
    constexpr int bottomPadding = 9;
    constexpr int iconSize = 48;
    constexpr int iconTextGap = 10;

    QFont bodyFont = QGuiApplication::font();
    bodyFont.setBold(false);
    bodyFont.setPointSizeF(9.5);
    QFontMetrics bodyMetrics(bodyFont);

    const bool hasIcon = !resolveNotificationIcons(data).content.isNull();
    const int bodyWidth = popupWidth - leftPadding - rightPadding - (hasIcon ? iconSize + iconTextGap : 0);

    int bodyHeight = 0;
    if (!data.body.trimmed().isEmpty()) {
        const QRect bounds = bodyMetrics.boundingRect(
            QRect(0, 0, bodyWidth, 2000),
            Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            data.body);
        bodyHeight = qMax(bodyMetrics.lineSpacing(), bounds.height());
    }

    int calculated = bodyHeight > 0
        ? bodyTop + bodyHeight + bottomPadding
        : titleTop + titleHeight + bottomPadding;

    if (hasIcon)
        calculated = qMax(calculated, titleTop + iconSize + bottomPadding);

    return qMax(minHeight, calculated);
}

class NotificationOverlay : public QWindow
{
public:
    NotificationOverlay(
        QScreen *screen,
        int stackMargin,
        const NotificationData &data,
        const AppConfig &config,
        std::function<void(NotificationOverlay *)> onClose)
        : backingStore(this),
          notification(data),
          config(config),
          onClose(std::move(onClose)),
          remainingMs(qMax(1, data.remainingMs)),
          totalLifetimeMs(qMax(1, data.totalLifetimeMs))
    {
        popupHeight = calculatePopupHeight(notification, config.width);
        notificationIcons = resolveNotificationIcons(notification);

        setSurfaceType(QWindow::RasterSurface);

        QSurfaceFormat surfaceFormat = format();
        surfaceFormat.setAlphaBufferSize(8);
        setFormat(surfaceFormat);

        setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        setScreen(screen);
        resize(config.width, popupHeight);

        layerWindow = LayerShellQt::Window::get(this);

        const bool anchorTop = config.position.startsWith(QStringLiteral("top-"));
        const bool anchorLeft = config.position.endsWith(QStringLiteral("-left"));

        LayerShellQt::Window::Anchors anchors;
        anchors.setFlag(
            anchorTop
                ? LayerShellQt::Window::AnchorTop
                : LayerShellQt::Window::AnchorBottom);
        anchors.setFlag(
            anchorLeft
                ? LayerShellQt::Window::AnchorLeft
                : LayerShellQt::Window::AnchorRight);

        layerWindow->setAnchors(anchors);
        layerWindow->setLayer(LayerShellQt::Window::LayerTop);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layerWindow->setExclusiveZone(-1);
        layerWindow->setDesiredSize(QSize(config.width, popupHeight));

        const int left = anchorLeft ? config.leftMargin : 0;
        const int right = anchorLeft ? 0 : config.rightMargin;
        const int top = anchorTop ? stackMargin : 0;
        const int bottom = anchorTop ? 0 : stackMargin;

        layerWindow->setMargins(QMargins(left, top, right, bottom));

        hideTimer.setSingleShot(true);
        QObject::connect(&hideTimer, &QTimer::timeout, this, [this]() { closeOverlay(); });

        repaintTimer.setInterval(33);
        QObject::connect(&repaintTimer, &QTimer::timeout, this, [this]() {
            if (!closing && isVisible())
                requestUpdate();
        });
    }

    void showNotification()
    {
        if (notification.title.trimmed().isEmpty()
            && notification.body.trimmed().isEmpty()
            && notification.appName.trimmed().isEmpty())
            return;

        show();
        requestUpdate();
        resumeCountdown();
    }

    NotificationData currentData() const
    {
        NotificationData data = notification;
        data.remainingMs = currentRemainingMs();
        data.totalLifetimeMs = totalLifetimeMs;
        return data;
    }

    void detachAndHide()
    {
        closing = true;
        hideTimer.stop();
        repaintTimer.stop();
        hide();
        onClose = nullptr;
    }

    void closeOverlay()
    {
        if (closing)
            return;
        closing = true;
        hideTimer.stop();
        repaintTimer.stop();
        hide();
        if (onClose)
            onClose(this);
    }

protected:
    void exposeEvent(QExposeEvent *) override
    {
        if (isExposed())
            render();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            render();
            return true;
        }
        if (event->type() == QEvent::Enter) {
            mouseInside = true;
            pauseCountdown();
            requestUpdate();
        }
        if (event->type() == QEvent::Leave) {
            mouseInside = false;
            closeHovered = false;
            resumeCountdown();
            requestUpdate();
        }
        return QWindow::event(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool newCloseHovered = closeButtonRect().contains(event->position().toPoint());
        if (newCloseHovered != closeHovered) {
            closeHovered = newCloseHovered;
            requestUpdate();
        }
        QWindow::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton
            && closeButtonRect().contains(event->position().toPoint())) {
            closeOverlay();
            return;
        }
        QWindow::mousePressEvent(event);
    }

private:
    QBackingStore backingStore;
    QTimer hideTimer;
    QTimer repaintTimer;
    QElapsedTimer runElapsed;
    LayerShellQt::Window *layerWindow = nullptr;
    NotificationData notification;
    AppConfig config;
    NotificationIcons notificationIcons;
    std::function<void(NotificationOverlay *)> onClose;
    int remainingMs = 5000;
    int totalLifetimeMs = 5000;
    int popupHeight = 82;
    bool mouseInside = false;
    bool closeHovered = false;
    bool closing = false;
    bool paused = true;

    QRect closeButtonRect() const
    {
        return QRect(width() - 32, 4, 22, 19);
    }

    QString elide(const QString &text, const QFont &font, int width) const
    {
        return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
    }

    int currentRemainingMs() const
    {
        if (paused || !runElapsed.isValid())
            return qMax(1, remainingMs);
        return qMax(1, remainingMs - static_cast<int>(runElapsed.elapsed()));
    }

    void pauseCountdown()
    {
        if (closing || paused)
            return;
        remainingMs = currentRemainingMs();
        hideTimer.stop();
        repaintTimer.stop();
        runElapsed.invalidate();
        paused = true;
    }

    void resumeCountdown()
    {
        if (closing)
            return;
        remainingMs = qMax(1, remainingMs);
        paused = false;
        runElapsed.restart();
        hideTimer.start(remainingMs);
        if (!repaintTimer.isActive())
            repaintTimer.start();
    }

    void render()
    {
        if (!isExposed())
            return;

        constexpr int leftPadding = 14;
        constexpr int rightPadding = 14;
        constexpr int appTop = 5;
        constexpr int appHeight = 17;
        constexpr int progressY = 27;
        constexpr int progressHeight = 2;
        constexpr int titleTop = 34;
        constexpr int titleHeight = 20;
        constexpr int bodyTop = 55;
        constexpr int bottomPadding = 9;
        constexpr int iconSize = 48;
        constexpr int iconTextGap = 10;

        const QRect rect(QPoint(0, 0), size());
        backingStore.resize(size());
        backingStore.beginPaint(rect);
        QPainter painter(backingStore.paintDevice());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        const int backgroundAlpha =
            qRound(255.0 * config.backgroundOpacity / 100.0);
        painter.setBrush(QColor(35, 38, 41, backgroundAlpha));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRectF(0, 0, width(), height()), 9, 9);

        QFont appFont = painter.font();
        appFont.setBold(false);
        appFont.setPointSizeF(9.0);
        painter.setFont(appFont);
        painter.setPen(QColor(210, 210, 210));
        const QString appText = notification.appName.trimmed().isEmpty()
            ? QStringLiteral("Notification")
            : notification.appName.trimmed();
        constexpr int applicationIconSize = 16;
        constexpr int applicationIconGap = 6;
        const bool hasApplicationIcon = !notificationIcons.application.isNull();
        if (hasApplicationIcon)
            notificationIcons.application.paint(&painter,
                QRect(leftPadding, appTop, applicationIconSize, applicationIconSize));
        const int applicationTextLeft = leftPadding
            + (hasApplicationIcon ? applicationIconSize + applicationIconGap : 0);
        const QRect appRect(applicationTextLeft, appTop,
                            width() - applicationTextLeft - rightPadding - 26, appHeight);
        painter.drawText(appRect, Qt::AlignLeft | Qt::AlignVCenter,
                         elide(appText, appFont, appRect.width()));

        if (closeHovered) {
            painter.setBrush(QColor(255, 255, 255, 28));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(closeButtonRect(), 5, 5);
        }
        QFont closeFont = painter.font();
        closeFont.setBold(false);
        closeFont.setPointSizeF(12.0);
        painter.setFont(closeFont);
        painter.setPen(closeHovered ? QColor(255, 255, 255) : QColor(230, 230, 230));
        painter.drawText(closeButtonRect(), Qt::AlignCenter, QStringLiteral("×"));

        painter.fillRect(QRect(0, progressY, width(), progressHeight), QColor(255, 255, 255, 55));
        const double ratio = static_cast<double>(currentRemainingMs())
            / static_cast<double>(qMax(1, totalLifetimeMs));
        const int fillWidth = qBound(0, static_cast<int>(width() * ratio), width());
        painter.fillRect(QRect(0, progressY, fillWidth, progressHeight), QColor(110, 130, 255, 235));

        const bool hasIcon = !notificationIcons.content.isNull();
        const int textLeft = leftPadding;
        const int textWidth = width() - textLeft - rightPadding - (hasIcon ? iconSize + iconTextGap : 0);

        if (hasIcon) {
            notificationIcons.content.paint(&painter,
                QRect(width() - rightPadding - iconSize, titleTop, iconSize, iconSize));
        }

        QFont titleFont = painter.font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(10.5);
        painter.setFont(titleFont);
        painter.setPen(QColor(245, 245, 245));
        const QRect titleRect(textLeft, titleTop, textWidth, titleHeight);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                         elide(notification.title, titleFont, titleRect.width()));

        QFont bodyFont = painter.font();
        bodyFont.setBold(false);
        bodyFont.setPointSizeF(9.5);
        painter.setFont(bodyFont);
        painter.setPen(QColor(220, 220, 220));
        const QRect bodyRect(textLeft, bodyTop, textWidth,
                             height() - bodyTop - bottomPadding);
        painter.drawText(bodyRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, notification.body);

        painter.end();
        backingStore.endPaint();
        backingStore.flush(rect);
    }
};

class OverlayManager : public QObject
{
public:
    OverlayManager(QScreen *screen, const AppConfig &config, QObject *parent = nullptr)
        : QObject(parent), screen(screen), config(config)
    {
    }

    void upsertNotification(const NotificationData &incoming)
    {
        syncRemainingTimes();

        NotificationData data = incoming;
        data.remainingMs = config.lifetimeMs;
        data.totalLifetimeMs = config.lifetimeMs;

        int existingIndex = -1;
        if (data.notificationId != 0) {
            for (int i = 0; i < notifications.size(); ++i) {
                if (notifications.at(i).notificationId == data.notificationId) {
                    existingIndex = i;
                    break;
                }
            }
        }

        if (existingIndex >= 0)
            notifications[existingIndex] = data;
        else
            notifications.append(data);

        while (notifications.size() > config.maxNotifications)
            notifications.removeFirst();

        rebuildOverlays();
    }

    void closeNotification(quint32 notificationId)
    {
        if (notificationId == 0)
            return;

        syncRemainingTimes();

        for (int i = 0; i < notifications.size(); ++i) {
            if (notifications.at(i).notificationId == notificationId) {
                notifications.removeAt(i);
                rebuildOverlays();
                return;
            }
        }
    }

private:
    QScreen *screen;
    AppConfig config;
    QList<NotificationData> notifications;
    QList<NotificationOverlay *> overlays;
    bool rebuilding = false;

    void syncRemainingTimes()
    {
        const int count = qMin(notifications.size(), overlays.size());
        for (int i = 0; i < count; ++i)
            notifications[i] = overlays.at(i)->currentData();
    }

    void rebuildOverlays()
    {
        if (rebuilding)
            return;
        rebuilding = true;

        const QList<NotificationOverlay *> oldOverlays = overlays;
        overlays.clear();
        for (NotificationOverlay *overlay : oldOverlays) {
            overlay->detachAndHide();
            overlay->deleteLater();
        }

        const bool stackFromTop =
            config.position.startsWith(QStringLiteral("top-"));
        const int baseMargin =
            stackFromTop ? config.topMargin : config.bottomMargin;

        int accumulatedHeight = 0;
        for (int i = 0; i < notifications.size(); ++i) {
            const int stackMargin = baseMargin + accumulatedHeight;
            auto *overlay = new NotificationOverlay(
                screen,
                stackMargin,
                notifications.at(i),
                config,
                [this](NotificationOverlay *item) { overlayClosed(item); });
            overlays.append(overlay);
            overlay->showNotification();
            accumulatedHeight +=
                calculatePopupHeight(notifications.at(i), config.width)
                + config.gap;
        }

        rebuilding = false;
    }

    void overlayClosed(NotificationOverlay *item)
    {
        if (rebuilding)
            return;

        syncRemainingTimes();
        const int index = overlays.indexOf(item);
        if (index < 0 || index >= notifications.size())
            return;

        overlays.removeAt(index);
        notifications.removeAt(index);
        item->detachAndHide();
        item->deleteLater();
        rebuildOverlays();
    }
};

struct PendingNotify
{
    NotificationData data;
};

class NotificationBusMonitor : public QObject
{
public:
    explicit NotificationBusMonitor(const QList<OverlayManager *> &managers, QObject *parent = nullptr)
        : QObject(parent), managers(managers)
    {
    }

    ~NotificationBusMonitor() override
    {
        if (notifier)
            notifier->setEnabled(false);
        sd_bus_flush_close_unref(bus);
    }

    bool start()
    {
        int r = sd_bus_new(&bus);
        if (r < 0) {
            logBusError("Failed to allocate D-Bus connection", r);
            return false;
        }

        QByteArray address = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        if (address.isEmpty())
            address = QByteArray("unix:path=/run/user/") + QByteArray::number(getuid()) + QByteArray("/bus");

        r = sd_bus_set_address(bus, address.constData());
        if (r < 0) {
            logBusError("Failed to set session bus address", r);
            return false;
        }

        r = sd_bus_set_bus_client(bus, 1);
        if (r < 0) {
            logBusError("Failed to configure D-Bus client mode", r);
            return false;
        }

        r = sd_bus_set_monitor(bus, 1);
        if (r < 0) {
            logBusError("Failed to configure D-Bus monitor mode", r);
            return false;
        }

        r = sd_bus_start(bus);
        if (r < 0) {
            logBusError("Failed to connect to the session bus", r);
            return false;
        }

        if (!becomeMonitor())
            return false;

        const int fd = sd_bus_get_fd(bus);
        if (fd < 0) {
            logBusError("Failed to get D-Bus file descriptor", fd);
            return false;
        }

        notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        QObject::connect(notifier, &QSocketNotifier::activated, this, [this]() {
            drainBus();
        });

        drainBus();
        qInfo() << "D-Bus notification monitor is active.";
        return true;
    }

private:
    QList<OverlayManager *> managers;
    QHash<QString, PendingNotify> pendingCalls;
    sd_bus *bus = nullptr;
    QSocketNotifier *notifier = nullptr;

    static void logBusError(const char *message, int error)
    {
        qCritical().noquote() << QString::fromLatin1(message) + QStringLiteral(": ")
                              + QString::fromLocal8Bit(strerror(-error));
    }

    static QString callKey(const char *peer, uint64_t cookie)
    {
        return QString::fromUtf8(peer ? peer : "") + QLatin1Char('#') + QString::number(cookie);
    }

    bool becomeMonitor()
    {
        sd_bus_message *call = nullptr;
        sd_bus_message *reply = nullptr;
        sd_bus_error error = SD_BUS_ERROR_NULL;

        int r = sd_bus_message_new_method_call(
            bus,
            &call,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus.Monitoring",
            "BecomeMonitor");
        if (r < 0) {
            logBusError("Failed to create BecomeMonitor call", r);
            return false;
        }

        r = sd_bus_message_open_container(call, SD_BUS_TYPE_ARRAY, "s");
        if (r >= 0)
            r = sd_bus_message_append(call, "s", "type='method_call',interface='org.freedesktop.Notifications',member='Notify'");
        if (r >= 0)
            r = sd_bus_message_append(call, "s", "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'");
        if (r >= 0)
            r = sd_bus_message_append(call, "s", "type='method_return'");
        if (r >= 0)
            r = sd_bus_message_append(call, "s", "type='error'");
        if (r >= 0)
            r = sd_bus_message_close_container(call);
        if (r >= 0)
            r = sd_bus_message_append(call, "u", uint32_t(0));

        if (r < 0) {
            logBusError("Failed to build BecomeMonitor call", r);
            sd_bus_message_unref(call);
            return false;
        }

        r = sd_bus_call(bus, call, 0, &error, &reply);
        sd_bus_message_unref(call);
        sd_bus_message_unref(reply);

        if (r < 0) {
            const QString detail = error.message
                ? QString::fromUtf8(error.message)
                : QString::fromLocal8Bit(strerror(-r));
            qCritical().noquote() << "Failed to become a D-Bus monitor:" << detail;
            sd_bus_error_free(&error);
            return false;
        }

        sd_bus_error_free(&error);
        return true;
    }

    void drainBus()
    {
        for (;;) {
            sd_bus_message *message = nullptr;
            const int r = sd_bus_process(bus, &message);
            if (r < 0) {
                logBusError("D-Bus processing failed", r);
                QCoreApplication::exit(1);
                return;
            }
            if (r == 0)
                return;

            if (message) {
                handleMessage(message);
                sd_bus_message_unref(message);
            }
        }
    }

    int handleMessage(sd_bus_message *message)
    {
        if (sd_bus_message_is_method_call(message, "org.freedesktop.Notifications", "Notify")) {
            handleNotifyCall(message);
            return 0;
        }

        if (sd_bus_message_is_signal(message, "org.freedesktop.Notifications", "NotificationClosed")) {
            handleNotificationClosed(message);
            return 0;
        }

        uint8_t type = 0;
        if (sd_bus_message_get_type(message, &type) < 0)
            return 0;

        if (type == SD_BUS_MESSAGE_METHOD_RETURN)
            handleMethodReturn(message);
        else if (type == SD_BUS_MESSAGE_METHOD_ERROR)
            handleMethodError(message);

        return 0;
    }

    void handleNotifyCall(sd_bus_message *message)
    {
        if (sd_bus_message_rewind(message, true) < 0)
            return;

        PendingNotify pending;
        if (!readNotificationPayload(message, pending.data)) {
            qWarning() << "Ignoring malformed Notify() call.";
            return;
        }

        uint64_t cookie = 0;
        if (sd_bus_message_get_cookie(message, &cookie) < 0)
            return;

        const char *sender = sd_bus_message_get_sender(message);
        pendingCalls.insert(callKey(sender, cookie), pending);
    }

    void handleMethodReturn(sd_bus_message *message)
    {
        uint64_t replyCookie = 0;
        if (sd_bus_message_get_reply_cookie(message, &replyCookie) < 0)
            return;

        const char *destination = sd_bus_message_get_destination(message);
        const QString key = callKey(destination, replyCookie);
        auto it = pendingCalls.find(key);
        if (it == pendingCalls.end())
            return;

        if (sd_bus_message_rewind(message, true) < 0) {
            pendingCalls.erase(it);
            return;
        }

        uint32_t notificationId = 0;
        if (sd_bus_message_read(message, "u", &notificationId) < 0 || notificationId == 0) {
            pendingCalls.erase(it);
            return;
        }

        NotificationData data = it->data;
        data.notificationId = notificationId;
        pendingCalls.erase(it);
        resolveSenderDesktopEntry(data, QString::fromUtf8(destination ? destination : ""));

        for (OverlayManager *manager : managers)
            manager->upsertNotification(data);
    }

    void handleMethodError(sd_bus_message *message)
    {
        uint64_t replyCookie = 0;
        if (sd_bus_message_get_reply_cookie(message, &replyCookie) < 0)
            return;

        const char *destination = sd_bus_message_get_destination(message);
        pendingCalls.remove(callKey(destination, replyCookie));
    }

    void handleNotificationClosed(sd_bus_message *message)
    {
        if (sd_bus_message_rewind(message, true) < 0)
            return;

        uint32_t notificationId = 0;
        uint32_t reason = 0;
        if (sd_bus_message_read(message, "uu", &notificationId, &reason) < 0)
            return;

        for (OverlayManager *manager : managers)
            manager->closeNotification(notificationId);
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-notification-mirror"));

    initializeIconTheme();
    const AppConfig config = loadConfig();
    const QList<QScreen *> targets = selectTargetScreens(config);

    if (targets.isEmpty()) {
        qCritical() << "No target monitors are available.";
        return 1;
    }

    qInfo() << "Primary monitor:" << QGuiApplication::primaryScreen()->name();
    qInfo() << "Mirror mode:" << config.mode;

    QList<OverlayManager *> managers;
    for (QScreen *screen : targets) {
        qInfo() << "Mirror monitor:" << screen->name();
        managers.append(new OverlayManager(screen, config, &app));
    }

    NotificationBusMonitor monitor(managers, &app);
    if (!monitor.start())
        return 1;

    return app.exec();
}
