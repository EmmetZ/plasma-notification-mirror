#include "config.h"
#include "notification.h"
#include "notificationactions.h"
#include "notificationinhibition.h"
#include "popuplayout.h"

#include <QBackingStore>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSurfaceFormat>
#include <QSocketNotifier>
#include <QHash>
#include <QTimer>
#include <QWindow>

#include <LayerShellQt/Window>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <systemd/sd-bus.h>
#include <unistd.h>

static QList<QScreen *> selectTargetScreens(const AppConfig &config)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    QStringList names;
    for (QScreen *screen : screens)
        names.append(screen->name());
    const QScreen *primary = QGuiApplication::primaryScreen();
    const QStringList targets = selectTargetScreenNames(config, names, primary ? primary->name() : QString());
    QList<QScreen *> result;
    for (QScreen *screen : screens) {
        if (targets.contains(screen->name()))
            result.append(screen);
    }
    return result;
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
        const PopupPlacement &placement,
        const NotificationData &data,
        const PopupConfig &config,
        std::function<void(NotificationOverlay *)> onClose,
        std::function<void(const NotificationData &)> onActivate)
        : backingStore(this),
          notification(data),
          config(config),
          onClose(std::move(onClose)),
          onActivate(std::move(onActivate)),
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
        if (hasDefaultAction(notification))
            setCursor(Qt::PointingHandCursor);
        resize(config.width, popupHeight);

        layerWindow = LayerShellQt::Window::get(this);

        LayerShellQt::Window::Anchors anchors;
        anchors.setFlag(LayerShellQt::Window::AnchorTop, placement.anchors.testFlag(Qt::TopEdge));
        anchors.setFlag(LayerShellQt::Window::AnchorBottom, placement.anchors.testFlag(Qt::BottomEdge));
        anchors.setFlag(LayerShellQt::Window::AnchorLeft, placement.anchors.testFlag(Qt::LeftEdge));
        anchors.setFlag(LayerShellQt::Window::AnchorRight, placement.anchors.testFlag(Qt::RightEdge));
        layerWindow->setAnchors(anchors);
        layerWindow->setLayer(LayerShellQt::Window::LayerTop);
        layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layerWindow->setExclusiveZone(-1);
        layerWindow->setDesiredSize(QSize(config.width, popupHeight));

        layerWindow->setMargins(placement.margins);

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
        onActivate = nullptr;
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
        if (event->button() == Qt::LeftButton) {
            pressed = true;
            pressedClose = closeButtonRect().contains(event->position().toPoint());
            event->accept();
            return;
        }
        QWindow::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && pressed) {
            pressed = false;
            const QPoint point = event->position().toPoint();
            if (pressedClose && closeButtonRect().contains(point))
                closeOverlay();
            else if (!pressedClose && !closeButtonRect().contains(point)
                     && QRect(QPoint(0, 0), size()).contains(point)
                     && hasDefaultAction(notification) && onActivate)
                onActivate(notification);
            event->accept();
            return;
        }
        QWindow::mouseReleaseEvent(event);
    }

private:
    QBackingStore backingStore;
    QTimer hideTimer;
    QTimer repaintTimer;
    QElapsedTimer runElapsed;
    LayerShellQt::Window *layerWindow = nullptr;
    NotificationData notification;
    PopupConfig config;
    NotificationIcons notificationIcons;
    std::function<void(NotificationOverlay *)> onClose;
    std::function<void(const NotificationData &)> onActivate;
    bool pressed = false;
    bool pressedClose = false;
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
    OverlayManager(QScreen *screen, const PopupConfig &config,
                   NotificationActions *actions, QObject *parent = nullptr)
        : QObject(parent), screen(screen), config(config), actions(actions)
    {
    }

    ~OverlayManager() override
    {
        for (NotificationOverlay *overlay : overlays) {
            overlay->detachAndHide();
            delete overlay;
        }
    }

    void refreshLayout()
    {
        syncRemainingTimes();
        rebuildOverlays();
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

    void clearNotifications()
    {
        notifications.clear();
        rebuildOverlays();
    }

private:
    QPointer<QScreen> screen;
    PopupConfig config;
    NotificationActions *actions;
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
        if (rebuilding || !screen)
            return;
        rebuilding = true;

        const QList<NotificationOverlay *> oldOverlays = overlays;
        overlays.clear();
        for (NotificationOverlay *overlay : oldOverlays) {
            overlay->detachAndHide();
            overlay->deleteLater();
        }

        QList<int> heights;
        heights.reserve(notifications.size());
        for (const NotificationData &data : notifications)
            heights.append(calculatePopupHeight(data, config.width));
        const QList<PopupPlacement> placements = popupPlacements(config, heights, screen->geometry().height());

        for (int i = 0; i < notifications.size(); ++i) {
            auto *overlay = new NotificationOverlay(
                screen,
                placements.at(i),
                notifications.at(i),
                config,
                [this](NotificationOverlay *item) { overlayClosed(item); },
                [this](const NotificationData &data) { actions->invokeDefault(data); });
            overlays.append(overlay);
            overlay->showNotification();
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

class TargetScreenManager : public QObject
{
public:
    TargetScreenManager(const AppConfig &config, NotificationActions *actions, QObject *parent = nullptr)
        : QObject(parent), config(config), actions(actions)
    {
        updateTimer.setSingleShot(true);
        updateTimer.setInterval(100);
        connect(&updateTimer, &QTimer::timeout, this, [this]() { updateScreens(); });
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            watchScreen(screen);
            updateTimer.start();
        });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
            // Destroy layer surfaces during screenRemoved, before Qt moves
            // orphaned windows onto another output.
            delete screenManagers.take(screen);
            updateTimer.start();
        });
        connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen *primary) {
            delete screenManagers.take(primary);
            updateTimer.start();
        });
        for (QScreen *screen : QGuiApplication::screens())
            watchScreen(screen);
        updateScreens();
    }

    QList<OverlayManager *> managers() const { return screenManagers.values(); }

private:
    AppConfig config;
    NotificationActions *actions;
    QHash<QScreen *, OverlayManager *> screenManagers;
    QTimer updateTimer;

    void watchScreen(QScreen *screen)
    {
        auto refresh = [this]() { updateTimer.start(); };
        connect(screen, &QScreen::geometryChanged, this, refresh);
        connect(screen, &QScreen::availableGeometryChanged, this, refresh);
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, refresh);
    }

    void updateScreens()
    {
        const QList<QScreen *> targets = selectTargetScreens(config);
        for (QScreen *screen : screenManagers.keys()) {
            if (!targets.contains(screen))
                delete screenManagers.take(screen);
        }
        for (QScreen *screen : targets) {
            if (!screenManagers.contains(screen)) {
                qInfo() << "Mirror monitor:" << screen->name();
                screenManagers.insert(screen, new OverlayManager(screen, config.popupForScreen(screen->name()), actions, this));
            } else {
                screenManagers.value(screen)->refreshLayout();
            }
        }
        if (targets.isEmpty())
            qInfo() << "No target monitors available; waiting for a display change.";
    }
};

class NotificationBusMonitor : public QObject
{
public:
    explicit NotificationBusMonitor(TargetScreenManager &screens, QObject *parent = nullptr)
        : QObject(parent), screens(screens)
    {
        connect(&inhibition, &NotificationInhibition::blockingChanged, this, [this](bool blocked) {
            qInfo() << (blocked ? "Plasma notifications inhibited; mirroring paused."
                               : "Plasma notifications enabled; mirroring resumed.");
            if (blocked) {
                // Never replay in-flight or visible notifications after leaving DND.
                pendingCalls.clear();
                for (OverlayManager *manager : this->screens.managers())
                    manager->clearNotifications();
            }
        });
    }

    ~NotificationBusMonitor() override
    {
        if (notifier)
            notifier->setEnabled(false);
        sd_bus_flush_close_unref(bus);
    }

    bool start()
    {
        inhibition.start();
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
    TargetScreenManager &screens;
    NotificationInhibition inhibition;
    QHash<QString, NotificationData> pendingCalls;
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
        if (inhibition.blocksPopups())
            return;
        if (sd_bus_message_rewind(message, true) < 0)
            return;

        NotificationData data;
        if (!readNotificationPayload(message, data)) {
            qWarning() << "Ignoring malformed Notify() call.";
            return;
        }

        uint64_t cookie = 0;
        if (sd_bus_message_get_cookie(message, &cookie) < 0)
            return;

        const char *sender = sd_bus_message_get_sender(message);
        pendingCalls.insert(callKey(sender, cookie), data);
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

        NotificationData data = it.value();
        data.notificationId = notificationId;
        pendingCalls.erase(it);
        resolveSenderDesktopEntry(data, QString::fromUtf8(destination ? destination : ""));

        for (OverlayManager *manager : screens.managers())
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

        for (OverlayManager *manager : screens.managers())
            manager->closeNotification(notificationId);
    }
};

int main(int argc, char **argv)
{
    QGuiApplication::setDesktopFileName(QStringLiteral("plasma-notification-mirror"));
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plasma-notification-mirror"));

    app.setQuitOnLastWindowClosed(false);
    initializeIconTheme();
    const AppConfig config = loadConfig();
    qInfo() << "Primary monitor:"
            << (QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->name() : QString());
    qInfo() << "Mirror mode:" << config.mode;

    NotificationActions actions;
    TargetScreenManager screens(config, &actions);
    actions.onInvoked = [&screens](quint32 id) {
        for (OverlayManager *manager : screens.managers())
            manager->closeNotification(id);
    };
    NotificationBusMonitor monitor(screens, &app);
    if (!monitor.start())
        return 1;

    return app.exec();
}
