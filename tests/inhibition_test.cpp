#include "notificationinhibition.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QTest>

namespace {
const QString service = QStringLiteral("org.freedesktop.Notifications");
const QString path = QStringLiteral("/org/freedesktop/Notifications");
const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");
}

class FakeNotifications : public QDBusVirtualObject
{
public:
    bool inhibited = false;
    bool holdReads = false;
    int reads = 0;
    QList<QDBusMessage> held;

    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override
    {
        if (message.interface() != properties || message.member() != QStringLiteral("Get"))
            return false;
        if (message.arguments() != QVariantList({service, QStringLiteral("Inhibited")}))
            return false;
        ++reads;
        if (holdReads)
            held.append(message);
        else
            bus.send(message.createReply({QVariant::fromValue(QDBusVariant(inhibited))}));
        return true;
    }

    void sendProperties(const QVariantMap &changed, const QStringList &invalidated = {},
                        const QString &interface = service)
    {
        auto signal = QDBusMessage::createSignal(path, properties, QStringLiteral("PropertiesChanged"));
        signal.setArguments({interface, changed, invalidated});
        QVERIFY(QDBusConnection::sessionBus().send(signal));
    }

    void setInhibited(bool value)
    {
        inhibited = value;
        sendProperties({{QStringLiteral("Inhibited"), value}});
    }
};

class InhibitionTest : public QObject
{
    Q_OBJECT
    FakeNotifications server;

private slots:
    void init()
    {
        server.inhibited = false;
        server.holdReads = false;
        server.reads = 0;
        server.held.clear();
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerVirtualObject(path, &server));
        QVERIFY(bus.registerService(service));
    }

    void cleanup()
    {
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.unregisterService(service));
        bus.unregisterObject(path);
    }

    void startup_data()
    {
        QTest::addColumn<bool>("inhibited");
        QTest::newRow("already-in-dnd") << true;
        QTest::newRow("notifications-enabled") << false;
    }

    void startup()
    {
        QFETCH(bool, inhibited);
        server.inhibited = inhibited;
        NotificationInhibition state;
        QVERIFY(state.blocksPopups());
        state.start();
        QTRY_COMPARE(server.reads, 1);
        QTRY_VERIFY(state.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QCOMPARE(state.blocksPopups(), inhibited);
    }

    void liveChanges()
    {
        NotificationInhibition state;
        QSignalSpy changes(&state, &NotificationInhibition::blockingChanged);
        state.start();
        QTRY_VERIFY(!state.blocksPopups());
        server.setInhibited(true);
        QTRY_VERIFY(state.blocksPopups());
        server.setInhibited(false);
        QTRY_VERIFY(!state.blocksPopups());
        QCOMPARE(changes.size(), 3);
        server.sendProperties({{QStringLiteral("Inhibited"), true}}, {}, QStringLiteral("org.example.Other"));
        server.sendProperties({{QStringLiteral("OtherProperty"), true}});
        QTest::qWait(30);
        QVERIFY(!state.blocksPopups());
        QCOMPARE(changes.size(), 3);
    }

    void invalidatedProperty()
    {
        NotificationInhibition state;
        state.start();
        QTRY_VERIFY(!state.blocksPopups());
        server.holdReads = true;
        server.inhibited = true;
        server.sendProperties({}, {QStringLiteral("Inhibited")});
        QTRY_COMPARE(server.held.size(), 1);
        QVERIFY(state.blocksPopups());
        auto request = server.held.takeFirst();
        QVERIFY(QDBusConnection::sessionBus().send(
            request.createReply({QVariant::fromValue(QDBusVariant(true))})));
        QTRY_VERIFY(state.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QVERIFY(state.blocksPopups());
        server.setInhibited(false);
        QTRY_VERIFY(!state.blocksPopups());
    }

    void staleReadCannotOverrideSignal()
    {
        server.holdReads = true;
        NotificationInhibition state;
        state.start();
        QTRY_COMPARE(server.held.size(), 1);
        server.setInhibited(false);
        QTRY_VERIFY(!state.blocksPopups());
        server.setInhibited(true);
        QTRY_VERIFY(state.blocksPopups());
        auto request = server.held.takeFirst();
        QVERIFY(QDBusConnection::sessionBus().send(
            request.createReply({QVariant::fromValue(QDBusVariant(false))})));
        QTRY_VERIFY(state.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QVERIFY(state.blocksPopups());
    }

    void daemonRestart()
    {
        NotificationInhibition state;
        state.start();
        QTRY_VERIFY(!state.blocksPopups());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.unregisterService(service));
        QTRY_VERIFY(state.blocksPopups());
        server.inhibited = true;
        QVERIFY(bus.registerService(service));
        QTRY_COMPARE(server.reads, 2);
        QTRY_VERIFY(state.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QVERIFY(state.blocksPopups());
        server.setInhibited(false);
        QTRY_VERIFY(!state.blocksPopups());
    }
};

QTEST_GUILESS_MAIN(InhibitionTest)
#include "inhibition_test.moc"
