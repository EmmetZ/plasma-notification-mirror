#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

// Uses an ordinary Qt D-Bus connection: a BecomeMonitor connection cannot query
// properties. Unknown state blocks popups until Plasma confirms they are allowed.
class NotificationInhibition : public QObject
{
    Q_OBJECT
public:
    explicit NotificationInhibition(QObject *parent = nullptr);
    void start();
    bool blocksPopups() const { return blocked; }

signals:
    void blockingChanged(bool blocked);

private slots:
    void propertiesChanged(const QString &interface, const QVariantMap &changed,
                           const QStringList &invalidated);

private:
    void refresh();
    void updateState(bool value);
    bool blocked = true;
    quint64 revision = 0;
    QTimer retryTimer;
};
