#pragma once

#include <QHash>
#include <QStringList>

struct PopupConfig
{
    bool enabled = true;
    QString positionMode = QStringLiteral("grid");
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

struct AppConfig
{
    QString mode = QStringLiteral("all-secondary");
    QStringList screens;
    PopupConfig defaults;
    QHash<QString, PopupConfig> screenConfigs;

    PopupConfig popupForScreen(const QString &name) const;
};

// An empty path uses the environment override or the standard config location.
AppConfig loadConfig(const QString &path = {});
QStringList selectTargetScreenNames(const AppConfig &config,
                                   const QStringList &available,
                                   const QString &primary);
