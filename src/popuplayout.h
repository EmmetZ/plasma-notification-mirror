#pragma once

#include "config.h"

#include <QList>
#include <QMargins>
#include <Qt>

struct PopupPlacement
{
    Qt::Edges anchors;
    QMargins margins;
};

// Screen height and popup heights use logical pixels. A centered row centers
// the whole vertical stack; horizontal centering is handled by layer-shell.
// Margin mode always uses one of the original corner anchors on both axes.
QList<PopupPlacement> popupPlacements(const PopupConfig &config,
                                    const QList<int> &heights, int screenHeight);
