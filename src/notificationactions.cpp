#include "notificationactions.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>

bool hasDefaultAction(const NotificationData &data)
{
    for (qsizetype i = 0; i + 1 < data.actions.size(); i += 2) {
        if (data.actions.at(i) == QStringLiteral("default"))
            return true;
    }
    return false;
}

void NotificationActions::invokeDefault(const NotificationData &data)
{
    if (!data.notificationId || !hasDefaultAction(data) || pending.contains(data.notificationId))
        return;
    pending.insert(data.notificationId);

    // A BecomeMonitor connection cannot send messages. Qt DBus uses a separate
    // ordinary connection, and Plasma emits the action with its own bus identity.
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.kde.NotificationManager"), QStringLiteral("InvokeAction"));
    call.setArguments({data.notificationId, QStringLiteral("default")});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, data](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        pending.remove(data.notificationId);
        if (reply.isError()) {
            qWarning() << "Could not invoke notification action:" << reply.error().message();
            return;
        }
        if (!data.resident) {
            QDBusMessage close = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("CloseNotification"));
            close.setArguments({data.notificationId});
            auto *closeWatcher = new QDBusPendingCallWatcher(
                QDBusConnection::sessionBus().asyncCall(close, 3000), this);
            connect(closeWatcher, &QDBusPendingCallWatcher::finished, this,
                    [](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<> reply = *watcher;
                if (reply.isError())
                    qWarning() << "Could not close activated notification:" << reply.error().message();
                watcher->deleteLater();
            });
            if (onInvoked)
                onInvoked(data.notificationId);
        }
    });
}
