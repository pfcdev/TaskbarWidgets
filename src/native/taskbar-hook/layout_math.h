#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

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

struct HorizontalSpan {
    double start;
    double end;
};

struct ScreenRectangle {
    double left;
    double top;
    double right;
    double bottom;
};

inline std::optional<HorizontalSpan> ProjectScreenObstacleToCanvas(
    const ScreenRectangle& obstacle,
    const ScreenRectangle& taskbarBand,
    double canvasScreenLeft,
    double dpiScale,
    double canvasWidth,
    double clearance) noexcept {
    const double width = obstacle.right - obstacle.left;
    const double height = obstacle.bottom - obstacle.top;
    if (width <= 0.0 || height <= 0.0 || dpiScale <= 0.0 ||
        canvasWidth <= 0.0) {
        return std::nullopt;
    }

    const double verticalOverlap =
        std::min(obstacle.bottom, taskbarBand.bottom) -
        std::max(obstacle.top, taskbarBand.top);
    if (verticalOverlap <= 0.0 || verticalOverlap * 2.0 < height) {
        return std::nullopt;
    }

    const double safeClearance = std::max(0.0, clearance);
    HorizontalSpan projected{
        (obstacle.left - canvasScreenLeft) / dpiScale - safeClearance,
        (obstacle.right - canvasScreenLeft) / dpiScale + safeClearance};
    if (projected.end <= 0.0 || projected.start >= canvasWidth) {
        return std::nullopt;
    }
    return projected;
}

enum class RuntimeControlSignal {
    Shutdown,
    Load,
};

enum class RuntimeControlAction {
    None,
    Stop,
    Start,
};

inline RuntimeControlAction RuntimeActionForSignal(
    bool runtimeActive,
    RuntimeControlSignal signal) noexcept {
    if (signal == RuntimeControlSignal::Shutdown) {
        return runtimeActive ? RuntimeControlAction::Stop
                             : RuntimeControlAction::None;
    }
    return runtimeActive ? RuntimeControlAction::None
                         : RuntimeControlAction::Start;
}

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

inline std::vector<HorizontalSpan> ComputeFreeGaps(
    double leftBound,
    double rightBound,
    std::vector<HorizontalSpan> obstacles) {
    std::vector<HorizontalSpan> gaps;
    if (rightBound <= leftBound) {
        return gaps;
    }

    std::vector<HorizontalSpan> blocked;
    blocked.reserve(obstacles.size());
    for (const auto& obstacle : obstacles) {
        const double start = std::max(leftBound, obstacle.start);
        const double end = std::min(rightBound, obstacle.end);
        if (end > start) {
            blocked.push_back({start, end});
        }
    }
    std::sort(blocked.begin(), blocked.end(), [](const auto& left, const auto& right) {
        return left.start == right.start ? left.end < right.end : left.start < right.start;
    });

    double cursor = leftBound;
    for (const auto& obstacle : blocked) {
        if (obstacle.start > cursor) {
            gaps.push_back({cursor, obstacle.start});
        }
        cursor = std::max(cursor, obstacle.end);
    }
    if (cursor < rightBound) {
        gaps.push_back({cursor, rightBound});
    }
    return gaps;
}

inline std::optional<double> PlaceInFittingGap(
    double preferredLeft,
    const std::vector<HorizontalSpan>& gaps,
    double width) noexcept {
    std::optional<double> best;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto& gap : gaps) {
        if (gap.end - gap.start < width) {
            continue;
        }

        const double candidate = std::clamp(
            preferredLeft,
            gap.start,
            gap.end - width);
        const double distance = std::abs(candidate - preferredLeft);
        if (distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

inline std::optional<double> PlaceAndReserve(
    double preferredLeft,
    double leftBound,
    double rightBound,
    double width,
    std::vector<HorizontalSpan>& obstacles,
    double clearance = 0.0) {
    const auto gaps = ComputeFreeGaps(leftBound, rightBound, obstacles);
    const auto placed = PlaceInFittingGap(preferredLeft, gaps, width);
    if (placed) {
        const double safeClearance = std::max(0.0, clearance);
        obstacles.push_back({
            *placed - safeClearance,
            *placed + width + safeClearance});
    }
    return placed;
}

inline std::optional<HorizontalSpan> SelectDragGap(
    const std::vector<HorizontalSpan>& gaps,
    double cursorX,
    double desiredLeft,
    double width,
    const std::optional<HorizontalSpan>& current) noexcept {
    std::optional<HorizontalSpan> underCursor;
    std::optional<HorizontalSpan> sticky;
    std::optional<HorizontalSpan> nearest;
    double nearestDistance = std::numeric_limits<double>::max();

    for (const auto& gap : gaps) {
        if (gap.end - gap.start < width) {
            continue;
        }
        if (cursorX >= gap.start && cursorX < gap.end) {
            underCursor = gap;
        }
        // Shell geometry can move by a pixel between samples. Matching the
        // previous lane by overlap prevents an accidental lane switch.
        if (current && gap.start < current->end && gap.end > current->start) {
            sticky = gap;
        }

        const double candidate = std::clamp(
            desiredLeft, gap.start, gap.end - width);
        const double distance = std::abs(candidate - desiredLeft);
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = gap;
        }
    }

    if (underCursor) return underCursor;
    if (sticky) return sticky;
    return nearest;
}

struct DragPlacement {
    double left;
    HorizontalSpan gap;
};

inline std::optional<DragPlacement> PlaceDuringDrag(
    double cursorX,
    double desiredLeft,
    double leftBound,
    double rightBound,
    double width,
    const std::vector<HorizontalSpan>& obstacles,
    const std::optional<HorizontalSpan>& current) {
    const auto gaps = ComputeFreeGaps(leftBound, rightBound, obstacles);
    const auto gap = SelectDragGap(
        gaps, cursorX, desiredLeft, width, current);
    if (!gap) {
        return std::nullopt;
    }
    return DragPlacement{
        std::clamp(desiredLeft, gap->start, gap->end - width),
        *gap};
}

}  // namespace taskbar_widgets
