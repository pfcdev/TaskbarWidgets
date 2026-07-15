#pragma once

#include <algorithm>

namespace taskbar_widgets {

struct PositionedWidget {
    double width;
    int anchorPercent;
    int offsetPx;
};

inline double LeftForWidget(const PositionedWidget& widget, double availableWidth) noexcept {
    const double anchor = availableWidth * std::clamp(widget.anchorPercent, 0, 100) / 100.0;
    return anchor - widget.width + std::clamp(widget.offsetPx, -640, 640);
}

inline double ClampHostWidth(double requested) noexcept {
    return std::clamp(requested, 1.0, 4096.0);
}

}  // namespace taskbar_widgets
