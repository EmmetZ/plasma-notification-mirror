#include "popuplayout.h"

#include <limits>

QList<PopupPlacement> popupPlacements(const PopupConfig &config,
                                    const QList<int> &heights, int screenHeight)
{
    const bool forceMargins = config.positionMode == QStringLiteral("margin");
    // Margin mode follows the original four-corner rule: always anchor both
    // axes and use edge distances, without any automatic centering.
    const bool bottom = forceMargins ? !config.position.startsWith(QStringLiteral("top-"))
                                    : config.position.startsWith(QStringLiteral("bottom-"));
    const bool centerRow = !forceMargins && (config.position == QStringLiteral("center")
        || config.position.startsWith(QStringLiteral("center-")));
    const bool left = config.position.endsWith(QStringLiteral("-left"));
    const bool right = forceMargins ? !left : config.position.endsWith(QStringLiteral("-right"));

    qint64 totalHeight = 0;
    for (int height : heights)
        totalHeight += height;
    if (!heights.isEmpty())
        totalHeight += qint64(heights.size() - 1) * config.gap;

    // If a centered stack is taller than the screen, start at the top rather
    // than placing its first popups above the screen.
    qint64 stackMargin = centerRow ? qMax<qint64>(0, (qint64(screenHeight) - totalHeight) / 2)
                                  : (bottom ? config.bottomMargin : config.topMargin);
    QList<PopupPlacement> placements;
    placements.reserve(heights.size());
    for (int height : heights) {
        PopupPlacement placement;
        placement.anchors = bottom ? Qt::BottomEdge : Qt::TopEdge;
        if (left) {
            placement.anchors |= Qt::LeftEdge;
            placement.margins.setLeft(config.leftMargin);
        } else if (right) {
            placement.anchors |= Qt::RightEdge;
            placement.margins.setRight(config.rightMargin);
        }
        // Leaving both horizontal anchors unset centers a fixed-width surface
        // on the output, with no manual width-dependent margin calculation.
        const int verticalMargin = int(qMin<qint64>(stackMargin, std::numeric_limits<int>::max()));
        if (bottom)
            placement.margins.setBottom(verticalMargin);
        else
            placement.margins.setTop(verticalMargin);
        placements.append(placement);
        stackMargin += qint64(height) + config.gap;
    }
    return placements;
}
