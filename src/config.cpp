#include "config.h"

#include <QDebug>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <limits>

namespace {
PopupConfig readPopupConfig(QSettings &settings, PopupConfig config)
{
    config.enabled = settings.value(QStringLiteral("enabled"), config.enabled).toBool();
    const QString position = settings.value(QStringLiteral("position"), config.position)
                                 .toString().trimmed().toLower();
    const QStringList validPositions = {QStringLiteral("top-left"), QStringLiteral("top-right"),
                                       QStringLiteral("bottom-left"), QStringLiteral("bottom-right")};
    if (validPositions.contains(position))
        config.position = position;
    else
        qWarning() << "Unknown position" << position << "; keeping" << config.position;

    auto number = [&settings](const char *key, int fallback, int minimum, int maximum) {
        bool ok = false;
        const int value = settings.value(QLatin1String(key), fallback).toInt(&ok);
        if (!ok) {
            qWarning() << "Invalid value for" << key << "; keeping" << fallback;
            return fallback;
        }
        return qBound(minimum, value, maximum);
    };
    config.lifetimeMs = number("lifetime_ms", config.lifetimeMs, 500, 60000);
    config.width = number("width", config.width, 220, 900);
    config.maxNotifications = number("max_notifications", config.maxNotifications, 1, 20);
    config.backgroundOpacity = number("background_opacity", config.backgroundOpacity, 0, 100);
    constexpr int maxMargin = std::numeric_limits<int>::max();
    config.leftMargin = number("left_margin", config.leftMargin, 0, maxMargin);
    config.rightMargin = number("right_margin", config.rightMargin, 0, maxMargin);
    config.topMargin = number("top_margin", config.topMargin, 0, maxMargin);
    config.bottomMargin = number("bottom_margin", config.bottomMargin, 0, maxMargin);
    config.gap = number("gap", config.gap, 0, 100);
    return config;
}
}

PopupConfig AppConfig::popupForScreen(const QString &name) const
{
    return screenConfigs.value(name, defaults);
}

AppConfig loadConfig(const QString &path)
{
    AppConfig config;
    QString configPath = path;
    if (configPath.isEmpty())
        configPath = qEnvironmentVariable("PLASMA_NOTIFICATION_MIRROR_CONFIG").trimmed();
    if (configPath.isEmpty()) {
        configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + QStringLiteral("/plasma-notification-mirror/config.ini");
    }
    if (!QFileInfo::exists(configPath)) {
        qInfo() << "No config file found; using defaults:" << configPath;
        return config;
    }
    qInfo() << "Loading config:" << configPath;
    QSettings settings(configPath, QSettings::IniFormat);
    // QSettings maps the INI [General] section to root keys.
    config.mode = settings.value(QStringLiteral("mode"), config.mode).toString().trimmed().toLower();
    const QVariant screenNames = settings.value(QStringLiteral("screens"));
    // INI comma-separated values can already be a QStringList.
    for (const QString &part : screenNames.toStringList()) {
        for (const QString &name : part.split(',', Qt::SkipEmptyParts)) {
            const QString trimmed = name.trimmed();
            if (!trimmed.isEmpty() && !config.screens.contains(trimmed))
                config.screens.append(trimmed);
        }
    }
    config.defaults = readPopupConfig(settings, config.defaults);
    for (const QString &group : settings.childGroups()) {
        const QString prefix = QStringLiteral("Screen:");
        if (!group.startsWith(prefix) || group.size() == prefix.size())
            continue;
        settings.beginGroup(group);
        config.screenConfigs.insert(group.mid(prefix.size()), readPopupConfig(settings, config.defaults));
        settings.endGroup();
    }
    if (config.mode == QStringLiteral("primary"))
        qWarning() << "mode=primary has no mirror targets: primary displays are always excluded.";
    else if (config.mode != QStringLiteral("secondary")
             && config.mode != QStringLiteral("all-secondary")
             && config.mode != QStringLiteral("all")
             && config.mode != QStringLiteral("screens")) {
        qWarning() << "Unknown mode" << config.mode << "; using 'all-secondary'.";
        config.mode = QStringLiteral("all-secondary");
    }
    return config;
}

QStringList selectTargetScreenNames(const AppConfig &config,
                                   const QStringList &available,
                                   const QString &primary)
{
    QStringList targets;
    if (config.mode == QStringLiteral("primary"))
        return targets;
    for (const QString &name : available) {
        if (name == primary || targets.contains(name) || !config.popupForScreen(name).enabled)
            continue;
        if (config.mode == QStringLiteral("screens") && !config.screens.contains(name))
            continue;
        targets.append(name);
        if (config.mode == QStringLiteral("secondary"))
            break;
    }
    return targets;
}
