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
    const QString positionMode = settings.value(QStringLiteral("position_mode"), config.positionMode)
                                     .toString().trimmed().toLower();
    if (positionMode == QStringLiteral("grid") || positionMode == QStringLiteral("margin"))
        config.positionMode = positionMode;
    else
        qWarning() << "Unknown position_mode" << positionMode << "; keeping" << config.positionMode;
    QString position = settings.value(QStringLiteral("position"), config.position)
                           .toString().trimmed().toLower();
    const QHash<QString, QString> aliases = {
        {QStringLiteral("top"), QStringLiteral("top-center")},
        {QStringLiteral("bottom"), QStringLiteral("bottom-center")},
        {QStringLiteral("left"), QStringLiteral("center-left")},
        {QStringLiteral("right"), QStringLiteral("center-right")},
        {QStringLiteral("left-center"), QStringLiteral("center-left")},
        {QStringLiteral("right-center"), QStringLiteral("center-right")},
        {QStringLiteral("middle-left"), QStringLiteral("center-left")},
        {QStringLiteral("middle-right"), QStringLiteral("center-right")},
        {QStringLiteral("middle"), QStringLiteral("center")},
        {QStringLiteral("center-center"), QStringLiteral("center")}
    };
    position = aliases.value(position, position);
    const QStringList corners = {
        QStringLiteral("top-left"), QStringLiteral("top-right"),
        QStringLiteral("bottom-left"), QStringLiteral("bottom-right")
    };
    const QStringList gridPositions = {
        QStringLiteral("top-left"), QStringLiteral("top-center"), QStringLiteral("top-right"),
        QStringLiteral("center-left"), QStringLiteral("center"), QStringLiteral("center-right"),
        QStringLiteral("bottom-left"), QStringLiteral("bottom-center"), QStringLiteral("bottom-right")
    };
    const QStringList &validPositions = config.positionMode == QStringLiteral("margin") ? corners : gridPositions;
    if (validPositions.contains(position))
        config.position = position;
    else {
        // Switching from an inherited centered grid position to margin mode
        // needs a usable corner even when no local position was supplied.
        if (!validPositions.contains(config.position))
            config.position = QStringLiteral("bottom-right");
        qWarning() << "Unsupported position" << position << "in" << config.positionMode
                   << "mode; using" << config.position;
    }

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
    // Apply a section's shorthand before its edge overrides. A per-screen
    // shorthand replaces inherited margins, then local edge keys take priority.
    if (settings.contains(QStringLiteral("margin"))) {
        bool ok = false;
        const int margin = settings.value(QStringLiteral("margin")).toInt(&ok);
        if (ok) {
            config.leftMargin = config.rightMargin = config.topMargin = config.bottomMargin = qMax(0, margin);
        } else {
            qWarning() << "Invalid margin; keeping inherited edge margins.";
        }
    }
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
