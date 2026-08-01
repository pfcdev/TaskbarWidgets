#include "layout_math.h"
#include "generated/widget_catalog.g.h"
#include "../common/json_string.h"

#include <cassert>
#include <string>

int main() {
    using taskbar_widgets::ClampHostWidth;
    using taskbar_widgets::LeftForWidget;
    using taskbar_widgets::IndependentLeftForWidget;
    using taskbar_widgets::HorizontalSpan;
    using taskbar_widgets::PositionForIndependentLeft;
    using taskbar_widgets::PositionedWidget;
    using taskbar_widgets::RuntimeControlAction;
    using taskbar_widgets::RuntimeControlSignal;
    using taskbar_widgets::ScreenRectangle;

    assert(LeftForWidget(PositionedWidget{200, 100, 0}, 1000) == 800);
    assert(LeftForWidget(PositionedWidget{200, 50, -20}, 1000) == 280);
    assert(LeftForWidget(PositionedWidget{200, 200, 900}, 1000) == 1440);
    assert(IndependentLeftForWidget(PositionedWidget{8, 100, -20}, 1000) == 972);
    assert(IndependentLeftForWidget(PositionedWidget{93, 50, 30}, 1000) == 483.5);
    assert(IndependentLeftForWidget(PositionedWidget{24, 0, -300}, 1000) == 0);
    const auto middlePosition = PositionForIndependentLeft(483.5, 93, 1000);
    assert(middlePosition.anchorPercent == 53);
    assert(middlePosition.offsetPx == 3);
    const auto restoredMiddle = IndependentLeftForWidget(
        PositionedWidget{93, middlePosition.anchorPercent, middlePosition.offsetPx}, 1000);
    assert(restoredMiddle > 483.0 && restoredMiddle < 484.0);
    const auto leftPosition = PositionForIndependentLeft(-20, 44, 1000);
    assert(leftPosition.anchorPercent == 0 && leftPosition.offsetPx == 0);
    const auto rightPosition = PositionForIndependentLeft(2000, 44, 1000);
    assert(rightPosition.anchorPercent == 100 && rightPosition.offsetPx == 0);
    assert(ClampHostWidth(-10) == 1);
    assert(ClampHostWidth(5000) == 4096);
    assert(taskbar_widgets::generated::kWidgets.size() == 10);
    auto cpu = std::find_if(
        taskbar_widgets::generated::kWidgets.begin(),
        taskbar_widgets::generated::kWidgets.end(),
        [](const auto& widget) { return widget.id == L"system-cpu"; });
    assert(cpu != taskbar_widgets::generated::kWidgets.end());
    assert(cpu->width == 32.0 && cpu->height == 24.0);
    auto parkingLot = std::find_if(
        taskbar_widgets::generated::kWidgets.begin(),
        taskbar_widgets::generated::kWidgets.end(),
        [](const auto& widget) { return widget.id == L"parking-lot"; });
    assert(parkingLot != taskbar_widgets::generated::kWidgets.end());
    assert(parkingLot->width == 64.0 && parkingLot->height == 32.0);
    assert(taskbar_widgets::SystemMeterWidth(L"bar", 8) == 85.0);
    assert(taskbar_widgets::SystemMeterWidth(L"pie", 4) == 105.0);
    assert(taskbar_widgets::SystemMeterWidth(L"text", 2) == 91.0);

    const auto gaps = taskbar_widgets::ComputeFreeGaps(
        0,
        1000,
        std::vector<HorizontalSpan>{{100, 200}, {180, 260}, {600, 700}});
    assert(gaps.size() == 3);
    assert(gaps[0].start == 0 && gaps[0].end == 100);
    assert(gaps[1].start == 260 && gaps[1].end == 600);
    assert(gaps[2].start == 700 && gaps[2].end == 1000);
    const auto nearestFit = taskbar_widgets::PlaceInFittingGap(580, gaps, 80);
    assert(nearestFit.has_value() && *nearestFit == 520);
    const auto exactFit = taskbar_widgets::PlaceInFittingGap(20, gaps, 100);
    assert(exactFit.has_value() && *exactFit == 0);
    const auto noFit = taskbar_widgets::PlaceInFittingGap(20, gaps, 350);
    assert(!noFit.has_value());

    // A taskbar icon cluster occupies [396, 604]. Widgets requested on top of
    // it must move into a free lane, then reserve their own lane so following
    // widgets cannot overlap them.
    std::vector<HorizontalSpan> taskbarObstacles{{396, 604}};
    const auto firstWidget = taskbar_widgets::PlaceAndReserve(
        480, 0, 1000, 100, taskbarObstacles, 4);
    assert(firstWidget.has_value() && *firstWidget == 604);
    const auto secondWidget = taskbar_widgets::PlaceAndReserve(
        610, 0, 1000, 100, taskbarObstacles, 4);
    assert(secondWidget.has_value() && *secondWidget == 708);
    assert(*firstWidget + 100 + 4 <= *secondWidget);
    const auto hiddenWidget = taskbar_widgets::PlaceAndReserve(
        500, 0, 1000, 400, taskbarObstacles, 4);
    assert(!hiddenWidget.has_value());

    // Live dragging uses the lane under the cursor. While the cursor crosses
    // an icon/widget obstacle, the dragged widget remains pinned in its current
    // lane; once the cursor reaches the far lane it transfers immediately.
    const std::vector<HorizontalSpan> centeredDragGaps{
        {0, 400}, {600, 1000}};
    const auto leftDragLane = taskbar_widgets::SelectDragGap(
        centeredDragGaps, 200, 150, 172, std::nullopt);
    assert(leftDragLane && leftDragLane->start == 0 && leftDragLane->end == 400);
    const auto pinnedAcrossIcons = taskbar_widgets::SelectDragGap(
        centeredDragGaps, 500, 450, 172, leftDragLane);
    assert(pinnedAcrossIcons && pinnedAcrossIcons->start == 0 &&
           pinnedAcrossIcons->end == 400);
    const double pinnedLeft = std::clamp(
        450.0, pinnedAcrossIcons->start, pinnedAcrossIcons->end - 172.0);
    assert(pinnedLeft == 228.0);
    const auto transferredPastIcons = taskbar_widgets::SelectDragGap(
        centeredDragGaps, 700, 650, 172, pinnedAcrossIcons);
    assert(transferredPastIcons && transferredPastIcons->start == 600 &&
           transferredPastIcons->end == 1000);
    const auto shiftedStickyLane = taskbar_widgets::SelectDragGap(
        std::vector<HorizontalSpan>{{0, 398}, {602, 1000}},
        500, 450, 172, leftDragLane);
    assert(shiftedStickyLane && shiftedStickyLane->start == 0 &&
           shiftedStickyLane->end == 398);
    const auto noDragLane = taskbar_widgets::SelectDragGap(
        std::vector<HorizontalSpan>{{0, 100}, {600, 700}},
        50, 20, 172, std::nullopt);
    assert(!noDragLane.has_value());

    const std::vector<HorizontalSpan> appIconObstacle{{396, 604}};
    const auto initialIconDrag = taskbar_widgets::PlaceDuringDrag(
        200, 114, 0, 1000, 172, appIconObstacle, std::nullopt);
    assert(initialIconDrag && initialIconDrag->left == 114 &&
           initialIconDrag->gap.end == 396);
    const auto pinnedAtAppIcons = taskbar_widgets::PlaceDuringDrag(
        500, 414, 0, 1000, 172, appIconObstacle, initialIconDrag->gap);
    assert(pinnedAtAppIcons && pinnedAtAppIcons->left == 224 &&
           pinnedAtAppIcons->left + 172 <= 396);
    const auto movedPastAppIcons = taskbar_widgets::PlaceDuringDrag(
        700, 614, 0, 1000, 172, appIconObstacle, pinnedAtAppIcons->gap);
    assert(movedPastAppIcons && movedPastAppIcons->left == 614 &&
           movedPastAppIcons->left >= 604);

    // Another widget uses the exact same live obstacle path as taskbar icons.
    const std::vector<HorizontalSpan> otherWidgetObstacle{{296, 404}};
    const auto beforeOtherWidget = taskbar_widgets::PlaceDuringDrag(
        180, 130, 0, 800, 100, otherWidgetObstacle, std::nullopt);
    assert(beforeOtherWidget && beforeOtherWidget->gap.end == 296);
    const auto pinnedAtOtherWidget = taskbar_widgets::PlaceDuringDrag(
        350, 300, 0, 800, 100, otherWidgetObstacle, beforeOtherWidget->gap);
    assert(pinnedAtOtherWidget && pinnedAtOtherWidget->left == 196 &&
           pinnedAtOtherWidget->left + 100 <= 296);
    const auto movedPastOtherWidget = taskbar_widgets::PlaceDuringDrag(
        500, 450, 0, 800, 100, otherWidgetObstacle,
        pinnedAtOtherWidget->gap);
    assert(movedPastOtherWidget && movedPastOtherWidget->left == 450 &&
           movedPastOtherWidget->left >= 404);

    // UI Automation reports physical screen pixels. Project all app-button
    // rectangles into the XAML canvas coordinate space, including non-100% DPI.
    const ScreenRectangle taskbarBand{0, 1000, 1920, 1060};
    const auto scaledAppButton =
        taskbar_widgets::ProjectScreenObstacleToCanvas(
            ScreenRectangle{600, 1000, 666, 1060},
            taskbarBand, 0, 1.5, 1280, 4);
    assert(scaledAppButton && scaledAppButton->start == 396 &&
           scaledAppButton->end == 448);
    const auto flyoutButton =
        taskbar_widgets::ProjectScreenObstacleToCanvas(
            ScreenRectangle{0, 500, 700, 950},
            taskbarBand, 0, 1.0, 1920, 4);
    assert(!flyoutButton.has_value());
    const auto offCanvasButton =
        taskbar_widgets::ProjectScreenObstacleToCanvas(
            ScreenRectangle{2000, 1000, 2044, 1060},
            taskbarBand, 0, 1.0, 1920, 4);
    assert(!offCanvasButton.has_value());

    // Disable stops only the runtime. The control loop and its load event stay
    // alive so Enable can restore widgets without restarting Explorer.
    assert(taskbar_widgets::RuntimeActionForSignal(
               true, RuntimeControlSignal::Shutdown) ==
           RuntimeControlAction::Stop);
    assert(taskbar_widgets::RuntimeActionForSignal(
               false, RuntimeControlSignal::Shutdown) ==
           RuntimeControlAction::None);
    assert(taskbar_widgets::RuntimeActionForSignal(
               false, RuntimeControlSignal::Load) ==
           RuntimeControlAction::Start);
    assert(taskbar_widgets::RuntimeActionForSignal(
               true, RuntimeControlSignal::Load) ==
           RuntimeControlAction::None);

    std::string decoded;
    assert(taskbar_widgets::json::ExtractStringUtf8(
        "{\"location\":\"\\u0130stanbul, \\u0130stanbul\"}", "location", decoded));
    assert(decoded == "\xC4\xB0stanbul, \xC4\xB0stanbul");
    assert(taskbar_widgets::json::ExtractStringUtf8(
        "{\"condition\":\"\\uD83C\\uDF24\"}", "condition", decoded));
    assert(decoded == "\xF0\x9F\x8C\xA4");
    assert(taskbar_widgets::json::ExtractStringUtf8(
        "{\"title\":\"A \\\"quoted\\\" title\"}", "title", decoded));
    assert(decoded == "A \"quoted\" title");
    assert(!taskbar_widgets::json::ExtractStringUtf8(
        "{\"location\":\"\\u013X\"}", "location", decoded));
    return 0;
}
