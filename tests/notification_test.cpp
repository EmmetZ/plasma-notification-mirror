#include "notification.h"
#include "notificationactions.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <memory>

struct BusDeleter { void operator()(sd_bus *bus) const { sd_bus_flush_close_unref(bus); } };
struct MessageDeleter { void operator()(sd_bus_message *message) const { sd_bus_message_unref(message); } };

class FakeServer : public QObject
{
public:
    QList<quint32> invoked;
    QList<quint32> closed;
};

class ActionAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.NotificationManager")
public:
    explicit ActionAdaptor(FakeServer *server) : QDBusAbstractAdaptor(server), server(server) {}
public slots:
    void InvokeAction(uint id, const QString &action)
    {
        QCOMPARE(action, QStringLiteral("default"));
        server->invoked.append(id);
    }
private:
    FakeServer *server;
};

class CloseAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    explicit CloseAdaptor(FakeServer *server) : QDBusAbstractAdaptor(server), server(server) {}
public slots:
    void CloseNotification(uint id) { server->closed.append(id); }
private:
    FakeServer *server;
};

class NotificationTest : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<sd_bus, BusDeleter> bus;
    FakeServer server;

    std::unique_ptr<sd_bus_message, MessageDeleter> messageWithImage(
        int width, int height, int stride, int alpha, int bits, int channels,
        const QByteArray &bytes, const char *key = "image-data")
    {
        sd_bus_message *raw = nullptr;
        if (sd_bus_message_new_method_call(bus.get(), &raw, "org.freedesktop.Notifications",
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify") < 0)
            return {};
        std::unique_ptr<sd_bus_message, MessageDeleter> message(raw);
        sd_bus_message_append(raw, "susss", "Image test", uint32_t(0), "", "Title", "Body");
        sd_bus_message_open_container(raw, 'a', "s");
        sd_bus_message_append(raw, "ssss", "default", "Open", "other", "Other action");
        sd_bus_message_close_container(raw);
        sd_bus_message_open_container(raw, 'a', "{sv}");
        sd_bus_message_append(raw, "{sv}", "unknown", "i", 42);
        sd_bus_message_open_container(raw, 'e', "sv");
        sd_bus_message_append(raw, "s", key);
        sd_bus_message_open_container(raw, 'v', "(iiibiiay)");
        sd_bus_message_open_container(raw, 'r', "iiibiiay");
        sd_bus_message_append(raw, "iiibii", width, height, stride, alpha, bits, channels);
        sd_bus_message_append_array(raw, 'y', bytes.constData(), size_t(bytes.size()));
        sd_bus_message_close_container(raw);
        sd_bus_message_close_container(raw);
        sd_bus_message_close_container(raw);
        sd_bus_message_append(raw, "{sv}", "desktop-entry", "s", "org.example.Test");
        sd_bus_message_append(raw, "{sv}", "x-kde-appname", "s", "test-component");
        sd_bus_message_append(raw, "{sv}", "resident", "b", 1);
        sd_bus_message_close_container(raw);
        sd_bus_message_append(raw, "i", -1);
        sd_bus_message_seal(raw, 1, 0);
        sd_bus_message_rewind(raw, true);
        return message;
    }

private slots:
    void initTestCase()
    {
        sd_bus *raw = nullptr;
        QVERIFY(sd_bus_open_user(&raw) >= 0);
        bus.reset(raw);
        auto dbus = QDBusConnection::sessionBus();
        QVERIFY(dbus.registerService(QStringLiteral("org.freedesktop.Notifications")));
        new ActionAdaptor(&server);
        new CloseAdaptor(&server);
        QVERIFY(dbus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server,
                                    QDBusConnection::ExportAdaptors));
    }

    void imageFormats_data()
    {
        QTest::addColumn<QByteArray>("key");
        QTest::addColumn<bool>("alpha");
        for (const char *key : {"image-data", "image_data", "icon_data"}) {
            QTest::newRow((QByteArray(key) + "-rgb").constData()) << QByteArray(key) << false;
            QTest::newRow((QByteArray(key) + "-rgba").constData()) << QByteArray(key) << true;
        }
    }

    void imageFormats()
    {
        QFETCH(QByteArray, key);
        QFETCH(bool, alpha);
        // Two padded RGB rows with no padding at the end of the final row.
        const QByteArray rgb = QByteArray::fromHex("ff000000ff00aaaaff000000ff00");
        const QByteArray rgba = QByteArray::fromHex("ff00008000ff00ffff00008000ff00ff");
        auto message = messageWithImage(2, 2, 8, alpha, 8, alpha ? 4 : 3,
                                        alpha ? rgba : rgb, key.constData());
        QVERIFY(message);
        NotificationData data;
        QVERIFY(readNotificationPayload(message.get(), data));
        const QImage image = key == "icon_data" ? data.legacyImage : data.image;
        QCOMPARE(image.size(), QSize(2, 2));
        QCOMPARE(image.pixelColor(0, 1), QColor(255, 0, 0, alpha ? 128 : 255));
        QCOMPARE(image.pixelColor(1, 1), QColor(0, 255, 0));
        QCOMPARE(data.desktopEntry, QStringLiteral("org.example.Test"));
        QCOMPARE(data.notifyRcName, QStringLiteral("test-component"));
        QVERIFY(data.resident);
        QVERIFY(hasDefaultAction(data));
        QVERIFY(!resolveNotificationIcons(data).content.isNull());
    }

    void malformedImages_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("stride");
        QTest::addColumn<int>("bits");
        QTest::addColumn<int>("channels");
        QTest::newRow("negative") << -1 << 2 << 8 << 8 << 4;
        QTest::newRow("huge") << 2147483647 << 2147483647 << 2147483647 << 8 << 4;
        QTest::newRow("short-stride") << 2 << 2 << 4 << 8 << 4;
        QTest::newRow("short-buffer") << 2 << 2 << 8 << 8 << 4;
        QTest::newRow("unsupported-bits") << 1 << 1 << 4 << 16 << 4;
        QTest::newRow("wrong-channels") << 1 << 1 << 4 << 8 << 3;
    }

    void malformedImages()
    {
        QFETCH(int, width); QFETCH(int, height); QFETCH(int, stride);
        QFETCH(int, bits); QFETCH(int, channels);
        auto message = messageWithImage(width, height, stride, 1, bits, channels, QByteArray(4, 'x'));
        QVERIFY(message);
        NotificationData data;
        QVERIFY(readNotificationPayload(message.get(), data));
        QVERIFY(data.image.isNull());
        // A bad image must not break later hints or default action parsing.
        QCOMPARE(data.desktopEntry, QStringLiteral("org.example.Test"));
        QVERIFY(hasDefaultAction(data));
    }

    void imagePriority()
    {
        NotificationData data;
        data.image = QImage(2, 2, QImage::Format_RGB888);
        data.image.fill(Qt::red);
        data.appIcon = QStringLiteral("dialog-information");
        QCOMPARE(resolveNotificationIcons(data).content.pixmap(2, 2).toImage().pixelColor(0, 0), QColor(Qt::red));
    }

    void defaultActionOnlyUsesKeys()
    {
        NotificationData data;
        data.actions = {QStringLiteral("other"), QStringLiteral("default")};
        QVERIFY(!hasDefaultAction(data));
    }

    void iconSearchPathsWithoutPlatformTheme()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldData = qgetenv("XDG_DATA_HOME");
        const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
        const auto oldPaths = QIcon::themeSearchPaths();
        const auto oldTheme = QIcon::themeName();
        const auto oldFallback = QIcon::fallbackThemeName();
        const auto restore = qScopeGuard([&] {
            if (oldData.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", oldData);
            if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", oldConfig);
            QIcon::setThemeName(oldTheme);
            QIcon::setFallbackThemeName(oldFallback);
            QIcon::setThemeSearchPaths(oldPaths);
        });
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", directory.filePath(QStringLiteral("config")).toUtf8());
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("config"))));
        QFile globals(directory.filePath(QStringLiteral("config/kdeglobals")));
        QVERIFY(globals.open(QIODevice::WriteOnly));
        globals.write("[Icons]\nTheme=mirror-test-theme\n");
        globals.close();
        const QString iconRoot = directory.filePath(QStringLiteral("icons/mirror-test-theme"));
        QVERIFY(QDir().mkpath(iconRoot + QStringLiteral("/16x16/apps")));
        QFile index(iconRoot + QStringLiteral("/index.theme"));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("[Icon Theme]\nName=Test\nDirectories=16x16/apps\n"
                    "[16x16/apps]\nSize=16\nType=Fixed\nContext=Applications\n");
        index.close();
        QImage image(16, 16, QImage::Format_RGB888);
        image.fill(Qt::green);
        QVERIFY(image.save(iconRoot + QStringLiteral("/16x16/apps/mirror-test-app.png")));
        QIcon::setThemeName(QStringLiteral("mirror-test-theme"));
        QIcon::setThemeSearchPaths({QStringLiteral(":/icons")});
        initializeIconTheme();
        QVERIFY(QIcon::themeSearchPaths().contains(directory.filePath(QStringLiteral("icons"))));
        const auto icon = QIcon::fromTheme(QStringLiteral("mirror-test-app"));
        QVERIFY(!icon.isNull());
        QCOMPARE(icon.pixmap(16, 16).toImage().pixelColor(0, 0), QColor(Qt::green));
    }

    void fileAndDesktopIcons()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString iconPath = directory.filePath(QStringLiteral("test.png"));
        QImage image(32, 32, QImage::Format_RGB888);
        image.fill(Qt::green);
        QVERIFY(image.save(iconPath));
        NotificationData data;
        data.appIcon = iconPath;
        QCOMPARE(resolveNotificationIcons(data).content.pixmap(32, 32).toImage().pixelColor(0, 0), QColor(Qt::green));
        data.appIcon.clear();
        data.imagePath = QUrl::fromLocalFile(iconPath).toString();
        QCOMPARE(resolveNotificationIcons(data).content.pixmap(32, 32).toImage().pixelColor(0, 0), QColor(Qt::green));

        const QByteArray previousDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const QString desktopPath = directory.filePath(QStringLiteral("applications/org.example.Icon.desktop"));
        QVERIFY(QDir().mkpath(QFileInfo(desktopPath).absolutePath()));
        QFile desktop(desktopPath);
        QVERIFY(desktop.open(QIODevice::WriteOnly));
        desktop.write("[Desktop Entry]\nType=Application\nName=Icon Test\nExec=true\nIcon=" + iconPath.toUtf8() + "\n");
        desktop.close();
        data.desktopEntry = desktopPath;
        // An existing but unreadable image must fall back to the desktop icon.
        const QString badPath = directory.filePath(QStringLiteral("bad.png"));
        QFile badFile(badPath);
        QVERIFY(badFile.open(QIODevice::WriteOnly));
        badFile.write("not an image");
        badFile.close();
        data.imagePath = badPath;
        const auto icons = resolveNotificationIcons(data);
        const QImage resolved = icons.application.pixmap(32, 32).toImage();
        QVERIFY(icons.content.isNull());
        if (previousDataHome.isNull())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", previousDataHome);
        QCOMPARE(resolved.pixelColor(0, 0), QColor(Qt::green));

        // Like Spectacle: the desktop application and content use the same icon.
        data.appIcon = iconPath;
        data.imagePath.clear();
        auto separated = resolveNotificationIcons(data);
        QVERIFY(!separated.application.isNull());
        QVERIFY(separated.content.isNull());

        // With a photo/avatar payload, app_icon belongs in the header instead.
        data.image = QImage(32, 32, QImage::Format_RGB888);
        data.image.fill(Qt::red);
        separated = resolveNotificationIcons(data);
        QCOMPARE(separated.application.pixmap(32, 32).toImage().pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(separated.content.pixmap(32, 32).toImage().pixelColor(0, 0), QColor(Qt::red));

        // notify-send's content icon alone must not become a header app icon.
        data.image = {};
        data.desktopEntry.clear();
        separated = resolveNotificationIcons(data);
        QVERIFY(separated.application.isNull());
        QCOMPARE(separated.content.pixmap(32, 32).toImage().pixelColor(0, 0), QColor(Qt::green));
    }

    void actions()
    {
        NotificationActions actions;
        QList<quint32> dismissed;
        actions.onInvoked = [&dismissed](quint32 id) { dismissed.append(id); };
        NotificationData data;
        data.notificationId = 71;
        data.actions = {QStringLiteral("default"), QStringLiteral("Open")};
        actions.invokeDefault(data);
        actions.invokeDefault(data); // double clicks must invoke only once while pending
        QTRY_COMPARE(server.invoked.count(71), 1);
        QTRY_VERIFY(server.closed.contains(71));
        QCOMPARE(dismissed.count(71), 1);

        data.notificationId = 72;
        data.resident = true;
        actions.invokeDefault(data);
        QTRY_VERIFY(server.invoked.contains(72));
        QTest::qWait(20);
        QVERIFY(!server.closed.contains(72));
        QVERIFY(!dismissed.contains(72));

        data.notificationId = 73;
        data.actions.clear();
        actions.invokeDefault(data);
        QTest::qWait(20);
        QVERIFY(!server.invoked.contains(73));
    }

    void failedActionCanRetry()
    {
        auto dbus = QDBusConnection::sessionBus();
        dbus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        NotificationActions actions;
        bool dismissed = false;
        actions.onInvoked = [&dismissed](quint32) { dismissed = true; };
        NotificationData data;
        data.notificationId = 74;
        data.actions = {QStringLiteral("default"), QStringLiteral("Open")};
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("Could not invoke notification action:.*"));
        actions.invokeDefault(data);
        QTRY_VERIFY(actions.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QVERIFY(!dismissed);
        QVERIFY(!server.closed.contains(74));
        QVERIFY(dbus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server,
                                    QDBusConnection::ExportAdaptors));
        actions.invokeDefault(data);
        QTRY_VERIFY(server.invoked.contains(74));
        QTRY_VERIFY(dismissed);
    }
};

QTEST_MAIN(NotificationTest)
#include "notification_test.moc"
