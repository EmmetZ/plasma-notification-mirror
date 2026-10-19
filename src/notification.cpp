#include "notification.h"

#include <QFileInfo>
#include <QDir>
#include <QDBusConnection>
#include <QDBusMessage>
#include <KApplicationTrader>
#include <KService>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <cstring>

void initializeIconTheme()
{
    // systemd services may start without the KDE platform theme plugin.
    const QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation,
                                               QStringLiteral("kdeglobals"));
    QString theme;
    if (!path.isEmpty()) {
        const QSettings settings(path, QSettings::IniFormat);
        theme = settings.value(QStringLiteral("Icons/Theme")).toString().trimmed();
    }
    if (!theme.isEmpty())
        QIcon::setThemeName(theme);
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("breeze"));
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    // systemd's environment may omit the Flatpak exports in XDG_DATA_DIRS.
    QStringList paths = QIcon::themeSearchPaths();
    QStringList roots = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    roots.append(QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share"));
    roots.append(QStringLiteral("/var/lib/flatpak/exports/share"));
    for (const QString &root : roots) {
        const QString icons = root + QStringLiteral("/icons");
        if (QFileInfo(icons).isDir() && !paths.contains(icons))
            paths.append(icons);
    }
    const QString legacyIcons = QDir::homePath() + QStringLiteral("/.icons");
    if (QFileInfo(legacyIcons).isDir() && !paths.contains(legacyIcons))
        paths.prepend(legacyIcons);
    QIcon::setThemeSearchPaths(paths);
}

static QIcon themedIcon(const QString &name)
{
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty())
        return {};

    QIcon icon = QIcon::fromTheme(cleaned);
    if (!icon.isNull())
        return icon;

    if (cleaned.endsWith(QStringLiteral(".desktop"))) {
        icon = QIcon::fromTheme(cleaned.chopped(8));
        if (!icon.isNull())
            return icon;
    }

    return {};
}

static QIcon iconFromPathOrTheme(const QString &value)
{
    const QString cleaned = value.trimmed();
    if (cleaned.isEmpty())
        return {};

    if (cleaned.startsWith(QStringLiteral("file://"))) {
        const QString path = QUrl(cleaned).toLocalFile();
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            const QIcon icon(path);
            if (!icon.pixmap(32, 32).isNull())
                return icon;
        }
    }

    if (QFileInfo(cleaned).isAbsolute() && QFileInfo::exists(cleaned)) {
        const QIcon icon(cleaned);
        if (!icon.pixmap(32, 32).isNull())
            return icon;
    }

    return themedIcon(cleaned);
}

static KService::Ptr findApplication(const NotificationData &data)
{
    QString entry = data.desktopEntry.trimmed();
    if (entry.endsWith(QStringLiteral(".desktop")))
        entry.chop(8);
    if (!entry.isEmpty()) {
        auto service = QFileInfo(entry).isAbsolute()
            ? KService::serviceByDesktopPath(data.desktopEntry)
            : KService::serviceByDesktopName(entry);
        // Explicit desktop paths can be newer than KDE's application cache.
        if (!service && QFileInfo(data.desktopEntry).isAbsolute() && QFileInfo::exists(data.desktopEntry)) {
            auto fromFile = KService::Ptr(new KService(data.desktopEntry));
            if (fromFile->isValid())
                service = fromFile;
        }
        if (!service)
            service = KService::serviceByDesktopName(entry.toLower());
        if (service)
            return service;
    }

    // Exact, case-insensitive identities also cover clients advertising a short
    // WM class (e.g. "wechat") instead of their exported Flatpak desktop ID.
    const auto matches = KApplicationTrader::query([&](const KService::Ptr &service) {
        if (!entry.isEmpty()) {
            if (service->desktopEntryName().compare(entry, Qt::CaseInsensitive) == 0
                || service->property<QString>(QStringLiteral("X-SnapInstanceName"))
                       .compare(entry, Qt::CaseInsensitive) == 0)
                return true;
            const QStringList renamed = service->property<QStringList>(QStringLiteral("X-Flatpak-RenamedFrom"));
            if (renamed.contains(entry + QStringLiteral(".desktop")))
                return true;
        }
        const QString appName = data.appName.trimmed();
        if (appName.isEmpty())
            return false;
        return service->name().compare(appName, Qt::CaseInsensitive) == 0
            || service->desktopEntryName().compare(appName, Qt::CaseInsensitive) == 0
            || service->property<QString>(QStringLiteral("StartupWMClass"))
                   .compare(appName, Qt::CaseInsensitive) == 0;
    });
    // Ambiguous display names must not pick an unrelated application.
    return matches.size() == 1 ? matches.first() : KService::Ptr();
}

void resolveSenderDesktopEntry(NotificationData &data, const QString &sender)
{
    if (sender.isEmpty() || (!data.desktopEntry.isEmpty() && findApplication(data)))
        return;
    auto call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("GetConnectionUnixProcessID"));
    call.setArguments({sender});
    const auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return;
    const uint pid = reply.arguments().first().toUInt();
    if (!pid)
        return;
    const QString path = QStringLiteral("/proc/%1/root/.flatpak-info").arg(pid);
    if (QFileInfo::exists(path)) {
        const QSettings settings(path, QSettings::IniFormat);
        data.desktopEntry = settings.value(QStringLiteral("Application/name")).toString();
    }
}

NotificationIcons resolveNotificationIcons(const NotificationData &data)
{
    QString applicationSource;
    if (const auto service = findApplication(data))
        applicationSource = service->icon();
    if (!data.notifyRcName.isEmpty() && !data.notifyRcName.contains(QLatin1Char('/'))) {
        const QString path = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
            QStringLiteral("knotifications6/") + data.notifyRcName + QStringLiteral(".notifyrc"));
        if (!path.isEmpty()) {
            const QSettings settings(path, QSettings::IniFormat);
            const QString configured = settings.value(QStringLiteral("Global/IconName")).toString();
            if (!configured.isEmpty()
                && (data.notifyRcName != QStringLiteral("plasma_workspace") || applicationSource.isEmpty()))
                applicationSource = configured;
        }
    }

    NotificationIcons icons;
    QString contentSource;
    const QImage pixels = !data.image.isNull() ? data.image : data.legacyImage;
    bool hasImage = !pixels.isNull();
    if (hasImage) {
        icons.content = QIcon(QPixmap::fromImage(pixels));
    } else {
        // KDE treats a local image hint as image data, and a theme name as an icon.
        icons.content = iconFromPathOrTheme(data.imagePath);
        contentSource = data.imagePath;
        hasImage = !icons.content.isNull()
            && (QFileInfo(data.imagePath).isAbsolute() || QUrl(data.imagePath).isLocalFile());
    }

    if (hasImage) {
        if (!data.appIcon.trimmed().isEmpty())
            applicationSource = data.appIcon;
    } else if (!data.appIcon.trimmed().isEmpty()) {
        // With no image payload, app_icon is the large notification content icon.
        contentSource = data.appIcon;
        icons.content = iconFromPathOrTheme(contentSource);
    }
    icons.application = iconFromPathOrTheme(applicationSource);
    if (!applicationSource.isEmpty() && contentSource == applicationSource)
        icons.content = {};
    return icons;
}

static QImage readImageData(sd_bus_message *message)
{
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "(iiibiiay)") <= 0)
        return {};
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "iiibiiay") <= 0) {
        sd_bus_message_exit_container(message);
        return {};
    }

    int32_t width = 0, height = 0, stride = 0, bits = 0, channels = 0;
    int alpha = 0;
    const void *bytes = nullptr;
    size_t byteCount = 0;
    int r = sd_bus_message_read(message, "iiibii", &width, &height, &stride,
                               &alpha, &bits, &channels);
    if (r > 0)
        r = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes, &byteCount);

    QImage image;
    // Bound dimensions and validate all arithmetic before exposing the buffer to Qt.
    // GdkPixbuf permits the final row to omit its trailing padding bytes.
    if (r > 0 && width > 0 && height > 0 && width <= 4096 && height <= 4096
        && bits == 8 && channels == (alpha ? 4 : 3)
        && stride >= qint64(width) * channels
        && qint64(stride) * (height - 1) + qint64(width) * channels <= qint64(byteCount)) {
        image = QImage(width, height, alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        if (!image.isNull()) {
            for (int y = 0; y < height; ++y)
                std::memcpy(image.scanLine(y), static_cast<const uchar *>(bytes) + qint64(y) * stride,
                            size_t(width) * channels);
        }
    }
    sd_bus_message_exit_container(message);
    sd_bus_message_exit_container(message);
    return image;
}

static bool readHints(sd_bus_message *message, NotificationData &data)
{
    int r = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r <= 0)
        return false;

    while ((r = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *keyRaw = nullptr;
        if (sd_bus_message_read(message, "s", &keyRaw) <= 0)
            return false;
        const QString key = QString::fromUtf8(keyRaw);
        const char *signature = nullptr;
        char type = 0;
        if (sd_bus_message_peek_type(message, &type, &signature) <= 0
            || type != SD_BUS_TYPE_VARIANT || !signature)
            return false;

        const bool imageData = key == QStringLiteral("image-data")
            || key == QStringLiteral("image_data") || key == QStringLiteral("icon_data");
        if (imageData && std::strcmp(signature, "(iiibiiay)") == 0) {
            const QImage image = readImageData(message);
            if (key == QStringLiteral("icon_data"))
                data.legacyImage = image;
            else if (key == QStringLiteral("image-data") || data.image.isNull())
                data.image = image;
        } else if (std::strcmp(signature, "s") == 0
                   && (key == QStringLiteral("image-path") || key == QStringLiteral("image_path")
                       || key == QStringLiteral("desktop-entry") || key == QStringLiteral("x-kde-appname"))) {
            if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "s") <= 0)
                return false;
            const char *value = nullptr;
            if (sd_bus_message_read(message, "s", &value) <= 0)
                return false;
            if (key == QStringLiteral("desktop-entry"))
                data.desktopEntry = QString::fromUtf8(value);
            else if (key == QStringLiteral("x-kde-appname"))
                data.notifyRcName = QString::fromUtf8(value);
            else
                data.imagePath = QString::fromUtf8(value);
            sd_bus_message_exit_container(message);
        } else if (sd_bus_message_skip(message, "v") < 0) {
            return false;
        }
        if (sd_bus_message_exit_container(message) < 0)
            return false;
    }
    return r == 0 && sd_bus_message_exit_container(message) >= 0;
}

bool readNotificationPayload(sd_bus_message *message, NotificationData &data)
{
    const char *appName = nullptr, *appIcon = nullptr, *title = nullptr, *body = nullptr;
    uint32_t replacesId = 0;
    if (sd_bus_message_read(message, "susss", &appName, &replacesId, &appIcon, &title, &body) <= 0)
        return false;
    data.appName = QString::fromUtf8(appName);
    data.appIcon = QString::fromUtf8(appIcon);
    data.title = QString::fromUtf8(title);
    data.body = QString::fromUtf8(body);

    if (sd_bus_message_skip(message, "as") < 0)
        return false;
    if (!readHints(message, data))
        return false;
    int32_t timeout = -1;
    return sd_bus_message_read(message, "i", &timeout) > 0;
}
