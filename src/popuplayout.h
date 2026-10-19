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
QList<PopupPlacement> popupPlacements(const PopupConfig &config,
                                    const QList<int> &heights, int screenHeight);
