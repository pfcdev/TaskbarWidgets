#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace taskbar_widgets {

struct PositionedWidget {
    double width;
    int anchorPercent;
    int offsetPx;
};

struct PersistedWidgetPosition {
    int anchorPercent;
    int offsetPx;
};

inline double LeftForWidget(const PositionedWidget& widget, double availableWidth) noexcept {
    const double anchor = availableWidth * std::clamp(widget.anchorPercent, 0, 100) / 100.0;
    return anchor - widget.width + std::clamp(widget.offsetPx, -640, 640);
}

inline double IndependentLeftForWidget(const PositionedWidget& widget,
                                       double availableWidth) noexcept {
    const double usableWidth = std::max(0.0, availableWidth - widget.width);
    const double requested = usableWidth * std::clamp(widget.anchorPercent, 0, 100) / 100.0 +
                             std::clamp(widget.offsetPx, -640, 640);
    return std::clamp(requested, 0.0, usableWidth);
}

inline PersistedWidgetPosition PositionForIndependentLeft(double left,
                                                           double widgetWidth,
                                                           double availableWidth) noexcept {
    const double usableWidth = std::max(0.0, availableWidth - widgetWidth);
    if (usableWidth <= 0.0) {
        return {100, 0};
    }

    const double clampedLeft = std::clamp(left, 0.0, usableWidth);
    const int anchorPercent = std::clamp(
        static_cast<int>(std::lround(clampedLeft * 100.0 / usableWidth)), 0, 100);
    const double anchorLeft = usableWidth * anchorPercent / 100.0;
    const int offsetPx = std::clamp(
        static_cast<int>(std::lround(clampedLeft - anchorLeft)), -640, 640);
    return {anchorPercent, offsetPx};
}

inline double ClampHostWidth(double requested) noexcept {
    return std::clamp(requested, 1.0, 4096.0);
}

inline double SystemMeterWidth(std::wstring_view mode, size_t count = 1) noexcept {
    count = std::max<size_t>(1, count);
    const double unit = mode == L"bar" ? 8.0 : mode == L"pie" ? 24.0 : 44.0;
    return unit * static_cast<double>(count) + 3.0 * static_cast<double>(count - 1);
}

}  // namespace taskbar_widgets
