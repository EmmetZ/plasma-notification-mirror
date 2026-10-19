#include "popuplayout.h"

#include <QTest>
#include <limits>

class LayoutTest : public QObject
{
    Q_OBJECT
private slots:
    void positions_data()
    {
        QTest::addColumn<QString>("position");
        QTest::addColumn<int>("anchors");
        QTest::addColumn<QMargins>("first");
        QTest::addColumn<QMargins>("second");
        QTest::newRow("top-left") << QString("top-left") << int(Qt::TopEdge | Qt::LeftEdge)
            << QMargins(18, 24, 0, 0) << QMargins(18, 114, 0, 0);
        QTest::newRow("top-center") << QString("top-center") << int(Qt::TopEdge)
            << QMargins(0, 24, 0, 0) << QMargins(0, 114, 0, 0);
        QTest::newRow("top-right") << QString("top-right") << int(Qt::TopEdge | Qt::RightEdge)
            << QMargins(0, 24, 20, 0) << QMargins(0, 114, 20, 0);
        QTest::newRow("center-left") << QString("center-left") << int(Qt::TopEdge | Qt::LeftEdge)
            << QMargins(18, 400, 0, 0) << QMargins(18, 490, 0, 0);
        QTest::newRow("center") << QString("center") << int(Qt::TopEdge)
            << QMargins(0, 400, 0, 0) << QMargins(0, 490, 0, 0);
        QTest::newRow("center-right") << QString("center-right") << int(Qt::TopEdge | Qt::RightEdge)
            << QMargins(0, 400, 20, 0) << QMargins(0, 490, 20, 0);
        QTest::newRow("bottom-left") << QString("bottom-left") << int(Qt::BottomEdge | Qt::LeftEdge)
            << QMargins(18, 0, 0, 88) << QMargins(18, 0, 0, 178);
        QTest::newRow("bottom-center") << QString("bottom-center") << int(Qt::BottomEdge)
            << QMargins(0, 0, 0, 88) << QMargins(0, 0, 0, 178);
        QTest::newRow("bottom-right") << QString("bottom-right") << int(Qt::BottomEdge | Qt::RightEdge)
            << QMargins(0, 0, 20, 88) << QMargins(0, 0, 20, 178);
    }

    void positions()
    {
        QFETCH(QString, position);
        QFETCH(int, anchors);
        QFETCH(QMargins, first);
        QFETCH(QMargins, second);
        PopupConfig config;
        config.position = position;
        config.leftMargin = 18;
        config.rightMargin = 20;
        config.topMargin = 24;
        config.bottomMargin = 88;
        config.gap = 10;
        const auto placements = popupPlacements(config, {80, 120, 60}, 1080);
        QCOMPARE(placements.size(), 3);
        for (const auto &placement : placements)
            QCOMPARE(int(placement.anchors), anchors);
        QCOMPARE(placements[0].margins, first);
        QCOMPARE(placements[1].margins, second);
    }

    void centeredStackReflows()
    {
        PopupConfig config;
        config.position = "center";
        config.gap = 10;
        // All edge margins are irrelevant for a fully centered stack.
        config.leftMargin = config.rightMargin = config.topMargin = config.bottomMargin = 5000;
        auto placements = popupPlacements(config, {80, 120}, 1000);
        QCOMPARE(placements[0].margins, QMargins(0, 395, 0, 0));
        QCOMPARE(placements[1].margins, QMargins(0, 485, 0, 0));
        // Closing/replacing a notification or changing screen size recenters the group.
        placements = popupPlacements(config, {120}, 1000);
        QCOMPARE(placements[0].margins, QMargins(0, 440, 0, 0));
        placements = popupPlacements(config, {120}, 1440);
        QCOMPARE(placements[0].margins, QMargins(0, 660, 0, 0));
        // Horizontal centering stays delegated to the compositor at any width.
        config.width = 800;
        placements = popupPlacements(config, {120}, 1440);
        QVERIFY(!placements[0].anchors.testFlag(Qt::LeftEdge));
        QVERIFY(!placements[0].anchors.testFlag(Qt::RightEdge));
    }

    void marginMode_data()
    {
        QTest::addColumn<QString>("corner");
        QTest::addColumn<int>("anchors");
        QTest::addColumn<QMargins>("first");
        QTest::addColumn<QMargins>("second");
        QTest::newRow("top-left") << QString("top-left") << int(Qt::TopEdge | Qt::LeftEdge)
            << QMargins(1118, 32, 0, 0) << QMargins(1118, 132, 0, 0);
        QTest::newRow("top-right") << QString("top-right") << int(Qt::TopEdge | Qt::RightEdge)
            << QMargins(0, 32, 600, 0) << QMargins(0, 132, 600, 0);
        QTest::newRow("bottom-left") << QString("bottom-left") << int(Qt::BottomEdge | Qt::LeftEdge)
            << QMargins(1118, 0, 0, 88) << QMargins(1118, 0, 0, 188);
        QTest::newRow("bottom-right") << QString("bottom-right") << int(Qt::BottomEdge | Qt::RightEdge)
            << QMargins(0, 0, 600, 88) << QMargins(0, 0, 600, 188);
    }

    void marginMode()
    {
        QFETCH(QString, corner);
        QFETCH(int, anchors);
        QFETCH(QMargins, first);
        QFETCH(QMargins, second);
        PopupConfig config;
        config.positionMode = "margin";
        config.position = corner;
        config.leftMargin = 1118;
        config.rightMargin = 600;
        config.topMargin = 32;
        config.bottomMargin = 88;
        config.gap = 20;
        auto placements = popupPlacements(config, {80, 120}, 1080);
        QCOMPARE(int(placements[0].anchors), anchors);
        QCOMPARE(int(placements[1].anchors), anchors);
        QCOMPARE(placements[0].margins, first);
        QCOMPARE(placements[1].margins, second);
        // Manual placement does not recenter after screen/popup dimensions change.
        config.width = 700;
        placements = popupPlacements(config, {80, 120}, 2160);
        QCOMPARE(placements[0].margins, first);
        QCOMPARE(placements[1].margins, second);
    }

    void emptyOverflowAndLargeMargins()
    {
        PopupConfig config;
        config.position = "center-left";
        QVERIFY(popupPlacements(config, {}, 100).isEmpty());
        auto placements = popupPlacements(config, {200, 200}, 100);
        QCOMPARE(placements[0].margins.top(), 0);
        QCOMPARE(placements[1].margins.top(), 212);
        config.position = "bottom-right";
        config.bottomMargin = std::numeric_limits<int>::max();
        placements = popupPlacements(config, {200, 200}, 100);
        QCOMPARE(placements[0].margins.bottom(), std::numeric_limits<int>::max());
        QCOMPARE(placements[1].margins.bottom(), std::numeric_limits<int>::max());
    }
};

QTEST_GUILESS_MAIN(LayoutTest)
#include "layout_test.moc"
