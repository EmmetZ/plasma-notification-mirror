#include "config.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class ConfigTest : public QObject
{
    Q_OBJECT
private slots:
    void defaults()
    {
        QTemporaryDir directory;
        const AppConfig config = loadConfig(directory.filePath(QStringLiteral("missing.ini")));
        QCOMPARE(config.mode, QStringLiteral("all-secondary"));
        QCOMPARE(config.defaults.positionMode, QStringLiteral("grid"));
        QCOMPARE(config.popupForScreen(QStringLiteral("DP-1")).width, 332);
        QCOMPARE(selectTargetScreenNames(config, {"eDP-1", "DP-1", "HDMI-A-1"}, "eDP-1"),
                 QStringList({"DP-1", "HDMI-A-1"}));
    }

    void independentSettings()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("config.ini"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nmode=screens\nscreens=DP-1, HDMI-A-1, DP-1\n"
                   "position=top-left\nwidth=400\nlifetime_ms=7000\nmax_notifications=8\n"
                   "background_opacity=80\nleft_margin=25\nright_margin=26\n"
                   "top_margin=27\nbottom_margin=28\ngap=17\n"
                   "[Screen:HDMI-A-1]\nposition=bottom-right\nwidth=600\nlifetime_ms=11000\n"
                   "max_notifications=2\nbackground_opacity=45\nleft_margin=100\n"
                   "right_margin=101\ntop_margin=102\nbottom_margin=103\ngap=30\n"
                   "[Screen:DP-1]\nwidth=300\n"
                   "[Screen:DP-2]\nenabled=false\n");
        file.close();
        const AppConfig config = loadConfig(path);
        QCOMPARE(config.screens, QStringList({"DP-1", "HDMI-A-1"}));
        const PopupConfig hdmi = config.popupForScreen("HDMI-A-1");
        QCOMPARE(hdmi.position, QStringLiteral("bottom-right"));
        QCOMPARE(hdmi.width, 600);
        QCOMPARE(hdmi.lifetimeMs, 11000);
        QCOMPARE(hdmi.maxNotifications, 2);
        QCOMPARE(hdmi.backgroundOpacity, 45);
        QCOMPARE(hdmi.leftMargin, 100);
        QCOMPARE(hdmi.rightMargin, 101);
        QCOMPARE(hdmi.topMargin, 102);
        QCOMPARE(hdmi.bottomMargin, 103);
        QCOMPARE(hdmi.gap, 30);
        const PopupConfig dp = config.popupForScreen("DP-1");
        QCOMPARE(dp.width, 300);
        QCOMPARE(dp.position, QStringLiteral("top-left"));
        QCOMPARE(dp.lifetimeMs, 7000);
        QCOMPARE(dp.maxNotifications, 8);
        QCOMPARE(dp.backgroundOpacity, 80);
        QCOMPARE(dp.leftMargin, 25);
        QCOMPARE(dp.rightMargin, 26);
        QCOMPARE(dp.topMargin, 27);
        QCOMPARE(dp.bottomMargin, 28);
        QCOMPARE(dp.gap, 17);
        QCOMPARE(config.defaults.width, 400);
        QCOMPARE(config.popupForScreen("new-screen").width, 400);
        QVERIFY(!config.popupForScreen("DP-2").enabled);
        // A local change cannot mutate another screen or the shared defaults.
        PopupConfig copy = config.popupForScreen("DP-1");
        copy.width = 800;
        QCOMPARE(config.popupForScreen("DP-1").width, 300);
        QCOMPARE(config.popupForScreen("HDMI-A-1").width, 600);
    }

    void validationAndInheritance()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("config.ini")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nposition=top-right\nwidth=420\n"
                   "[Screen:DP-1]\nposition=invalid\nwidth=invalid\n"
                   "lifetime_ms=1\nmax_notifications=999\nbackground_opacity=-5\n"
                   "left_margin=-1\nright_margin=5000\ngap=101\n");
        file.close();
        const PopupConfig popup = loadConfig(file.fileName()).popupForScreen("DP-1");
        QCOMPARE(popup.position, QStringLiteral("top-right"));
        QCOMPARE(popup.width, 420);
        QCOMPARE(popup.lifetimeMs, 500);
        QCOMPARE(popup.maxNotifications, 20);
        QCOMPARE(popup.backgroundOpacity, 0);
        QCOMPARE(popup.leftMargin, 0);
        QCOMPARE(popup.rightMargin, 5000);
        QCOMPARE(popup.gap, 100);
    }

    void targets_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("primary");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("all-secondary") << QString("all-secondary") << QString("eDP-1") << QStringList({"DP-1", "HDMI-A-1"});
        QTest::newRow("legacy-all") << QString("all") << QString("eDP-1") << QStringList({"DP-1", "HDMI-A-1"});
        QTest::newRow("legacy-primary-excluded") << QString("primary") << QString("eDP-1") << QStringList();
        QTest::newRow("first-secondary") << QString("secondary") << QString("eDP-1") << QStringList({"DP-1"});
        QTest::newRow("explicit-includes-primary") << QString("screens") << QString("eDP-1") << QStringList({"HDMI-A-1"});
        QTest::newRow("primary-switch") << QString("screens") << QString("HDMI-A-1") << QStringList({"eDP-1"});
    }

    void targets()
    {
        QFETCH(QString, mode);
        QFETCH(QString, primary);
        QFETCH(QStringList, expected);
        AppConfig config;
        config.mode = mode;
        config.screens = {"eDP-1", "HDMI-A-1", "missing"};
        PopupConfig disabled;
        disabled.enabled = false;
        config.screenConfigs.insert("disabled", disabled);
        QCOMPARE(selectTargetScreenNames(config, {"eDP-1", "disabled", "DP-1", "HDMI-A-1", "DP-1"}, primary), expected);
    }

    void hotplugAndDisabledScreens()
    {
        AppConfig config;
        config.mode = "screens";
        config.screens = {"HDMI-A-1"};
        PopupConfig hdmi;
        hdmi.width = 500;
        config.screenConfigs.insert("HDMI-A-1", hdmi);
        QVERIFY(selectTargetScreenNames(config, {"eDP-1", "DP-1"}, "eDP-1").isEmpty());
        QCOMPARE(selectTargetScreenNames(config, {"eDP-1", "DP-1", "HDMI-A-1"}, "eDP-1"), QStringList({"HDMI-A-1"}));
        QCOMPARE(config.popupForScreen("HDMI-A-1").width, 500);
        config.screenConfigs["HDMI-A-1"].enabled = false;
        QVERIFY(selectTargetScreenNames(config, {"eDP-1", "DP-1", "HDMI-A-1"}, "eDP-1").isEmpty());
        config.mode = "all-secondary";
        QCOMPARE(selectTargetScreenNames(config, {"eDP-1", "DP-1", "HDMI-A-1"}, "eDP-1"), QStringList({"DP-1"}));
        QVERIFY(selectTargetScreenNames(config, {"eDP-1"}, "eDP-1").isEmpty());
        QVERIFY(selectTargetScreenNames(config, {}, {}).isEmpty());
    }

    void enableOverride()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("config.ini")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nenabled=false\n"
                   "[Screen:DP-1]\nenabled=true\n"
                   "[Screen:eDP-1]\nenabled=true\n");
        file.close();
        const AppConfig config = loadConfig(file.fileName());
        QCOMPARE(selectTargetScreenNames(config, {"eDP-1", "DP-1", "HDMI-A-1"}, "eDP-1"),
                 QStringList({"DP-1"}));
    }

    void marginShorthand()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("config.ini")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nmargin=30\nleft_margin=40\nbottom_margin=60\n"
                   "[Screen:HDMI-A-1]\nmargin=12\nright_margin=18\n"
                   "[Screen:DP-1]\nposition=bottom-center\nbottom_margin=70\n"
                   "[Screen:DP-2]\nmargin=invalid\n"
                   "[Screen:DP-3]\nmargin=-10\n");
        file.close();
        const AppConfig config = loadConfig(file.fileName());
        const PopupConfig hdmi = config.popupForScreen("HDMI-A-1");
        QCOMPARE(hdmi.leftMargin, 12);
        QCOMPARE(hdmi.rightMargin, 18);
        QCOMPARE(hdmi.topMargin, 12);
        QCOMPARE(hdmi.bottomMargin, 12);
        const PopupConfig dp = config.popupForScreen("DP-1");
        QCOMPARE(dp.position, QStringLiteral("bottom-center"));
        QCOMPARE(dp.leftMargin, 40);
        QCOMPARE(dp.rightMargin, 30);
        QCOMPARE(dp.topMargin, 30);
        QCOMPARE(dp.bottomMargin, 70);
        QCOMPARE(config.popupForScreen("DP-2").bottomMargin, 60);
        QCOMPARE(config.popupForScreen("DP-2").leftMargin, 40);
        const PopupConfig zero = config.popupForScreen("DP-3");
        QCOMPARE(zero.leftMargin, 0);
        QCOMPARE(zero.rightMargin, 0);
        QCOMPARE(zero.topMargin, 0);
        QCOMPARE(zero.bottomMargin, 0);
        QCOMPARE(config.defaults.leftMargin, 40);
    }

    void positions_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("canonical");
        const QStringList positions = {"top-left", "top-center", "top-right", "center-left",
                                       "center", "center-right", "bottom-left", "bottom-center", "bottom-right"};
        for (const QString &position : positions)
            QTest::newRow(qPrintable(position)) << position << position;
        QTest::newRow("top-alias") << QString("top") << QString("top-center");
        QTest::newRow("bottom-alias") << QString("bottom") << QString("bottom-center");
        QTest::newRow("left-alias") << QString("left") << QString("center-left");
        QTest::newRow("right-alias") << QString("right") << QString("center-right");
        QTest::newRow("left-center-alias") << QString("left-center") << QString("center-left");
        QTest::newRow("right-center-alias") << QString("right-center") << QString("center-right");
        QTest::newRow("middle-left-alias") << QString("middle-left") << QString("center-left");
        QTest::newRow("middle-right-alias") << QString("middle-right") << QString("center-right");
        QTest::newRow("middle-alias") << QString("middle") << QString("center");
        QTest::newRow("center-center-alias") << QString("center-center") << QString("center");
        QTest::newRow("case-and-whitespace") << QString("  TOP-CENTER  ") << QString("top-center");
    }

    void positions()
    {
        QFETCH(QString, input);
        QFETCH(QString, canonical);
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("config.ini")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[General]\nposition=" + input.toUtf8() + "\n");
        file.close();
        QCOMPARE(loadConfig(file.fileName()).defaults.position, canonical);
    }

    void positionModes_data()
    {
        QTest::addColumn<QByteArray>("ini");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("position");
        QTest::newRow("margin-global-inherited")
            << QByteArray("[General]\nposition_mode=margin\nposition=top-left\n[Screen:DP-1]\nleft_margin=1118\n")
            << QString("margin") << QString("top-left");
        QTest::newRow("margin-per-screen")
            << QByteArray("[General]\nposition=bottom-center\n[Screen:DP-1]\nposition_mode=margin\nposition=bottom-left\n")
            << QString("margin") << QString("bottom-left");
        QTest::newRow("grid-per-screen")
            << QByteArray("[General]\nposition_mode=margin\n[Screen:DP-1]\nposition_mode=grid\nposition=center\n")
            << QString("grid") << QString("center");
        QTest::newRow("center-rejected-in-margin")
            << QByteArray("[General]\nposition_mode=margin\nposition=top-left\n[Screen:DP-1]\nposition=center\n")
            << QString("margin") << QString("top-left");
        QTest::newRow("center-inherited-when-switching")
            << QByteArray("[General]\nposition=bottom-center\n[Screen:DP-1]\nposition_mode=margin\n")
            << QString("margin") << QString("bottom-right");
        QTest::newRow("invalid-mode-keeps-inherited")
            << QByteArray("[General]\nposition_mode=margin\n[Screen:DP-1]\nposition_mode=invalid\nposition=bottom-left\n")
            << QString("margin") << QString("bottom-left");
        QTest::newRow("case-and-whitespace")
            << QByteArray("[General]\n[Screen:DP-1]\nposition_mode= MARGIN \nposition= TOP-RIGHT \n")
            << QString("margin") << QString("top-right");
    }

    void positionModes()
    {
        QFETCH(QByteArray, ini);
        QFETCH(QString, mode);
        QFETCH(QString, position);
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("config.ini")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(ini);
        file.close();
        const AppConfig config = loadConfig(file.fileName());
        const PopupConfig popup = config.popupForScreen("DP-1");
        QCOMPARE(popup.positionMode, mode);
        QCOMPARE(popup.position, position);
        // Local mode/position overrides do not alter other screens' defaults.
        QCOMPARE(config.popupForScreen("other").positionMode, config.defaults.positionMode);
        QCOMPARE(config.popupForScreen("other").position, config.defaults.position);
    }
};

QTEST_GUILESS_MAIN(ConfigTest)
#include "config_test.moc"
