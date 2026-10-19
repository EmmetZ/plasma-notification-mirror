#pragma once

#include "notification.h"

#include <QObject>
#include <QSet>
#include <functional>

bool hasDefaultAction(const NotificationData &data);

class NotificationActions : public QObject
{
public:
    explicit NotificationActions(QObject *parent = nullptr) : QObject(parent) {}

    void invokeDefault(const NotificationData &data);
    std::function<void(quint32)> onInvoked;

private:
    QSet<quint32> pending;
};
