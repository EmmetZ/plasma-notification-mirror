#include "notificationinhibition.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDebug>

namespace {
const QString service = QStringLiteral("org.freedesktop.Notifications");
const QString path = QStringLiteral("/org/freedesktop/Notifications");
const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");
const QString inhibited = QStringLiteral("Inhibited");
}

NotificationInhibition::NotificationInhibition(QObject *parent) : QObject(parent)
{
    retryTimer.setSingleShot(true);
    retryTimer.setInterval(3000);
    connect(&retryTimer, &QTimer::timeout, this, &NotificationInhibition::refresh);
}

void NotificationInhibition::start()
{
    auto bus = QDBusConnection::sessionBus();
    // Subscribe before reading, so a change during the initial query is not lost.
    if (!bus.connect(service, path, properties, QStringLiteral("PropertiesChanged"),
                     this, SLOT(propertiesChanged(QString,QVariantMap,QStringList)))) {
        qWarning() << "Could not watch Plasma notification inhibition; mirroring is paused.";
        return;
    }
    auto *watcher = new QDBusServiceWatcher(service, bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &owner) {
        updateState(true);
        if (!owner.isEmpty())
            refresh();
    });
    refresh();
}

void NotificationInhibition::updateState(bool value)
{
    // Every observation supersedes older reads and scheduled retries, even
    // when the blocking state itself has not changed.
    ++revision;
    retryTimer.stop();
    if (blocked == value)
        return;
    blocked = value;
    emit blockingChanged(blocked);
}

void NotificationInhibition::refresh()
{
    const quint64 requestRevision = ++revision;
    QDBusMessage call = QDBusMessage::createMethodCall(service, path, properties, QStringLiteral("Get"));
    call.setArguments({service, inhibited});
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, requestRevision](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        watcher->deleteLater();
        // A newer property signal or daemon owner takes precedence over this read.
        if (requestRevision != revision)
            return;
        if (reply.isError() || reply.value().variant().metaType() != QMetaType::fromType<bool>()) {
            updateState(true);
            qWarning() << "Could not read Plasma notification inhibition; mirroring is paused.";
            retryTimer.start();
            return;
        }
        qInfo() << "Plasma notification inhibition:" << reply.value().variant().toBool();
        updateState(reply.value().variant().toBool());
    });
}

void NotificationInhibition::propertiesChanged(const QString &interface, const QVariantMap &changed,
                                               const QStringList &invalidated)
{
    if (interface != service)
        return;
    const QVariant value = changed.value(inhibited);
    if (value.metaType() == QMetaType::fromType<bool>()) {
        updateState(value.toBool());
    } else if (invalidated.contains(inhibited) || changed.contains(inhibited)) {
        updateState(true);
        refresh();
    }
}
