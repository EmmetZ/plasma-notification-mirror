#pragma once

#include <QIcon>
#include <QImage>
#include <QStringList>
#include <systemd/sd-bus.h>

struct NotificationData
{
    quint32 notificationId = 0;
    QString appName;
    QString appIcon;
    QString imagePath;
    QImage image;
    QImage legacyImage;
    QString desktopEntry;
    QString notifyRcName;
    QString title;
    QString body;
    int remainingMs = 5000;
    int totalLifetimeMs = 5000;
};

struct NotificationIcons
{
    QIcon application;
    QIcon content;
};

NotificationIcons resolveNotificationIcons(const NotificationData &data);
void resolveSenderDesktopEntry(NotificationData &data, const QString &sender);
bool readNotificationPayload(sd_bus_message *message, NotificationData &data);
void initializeIconTheme();
