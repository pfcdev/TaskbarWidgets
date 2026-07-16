#include "layout_math.h"
#include "generated/widget_catalog.g.h"
#include "../common/json_string.h"

#include <cassert>
#include <string>

int main() {
    using taskbar_widgets::ClampHostWidth;
    using taskbar_widgets::LeftForWidget;
    using taskbar_widgets::IndependentLeftForWidget;
    using taskbar_widgets::PositionForIndependentLeft;
    using taskbar_widgets::PositionedWidget;

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
    assert(taskbar_widgets::generated::kWidgets.size() == 9);
    auto cpu = std::find_if(
        taskbar_widgets::generated::kWidgets.begin(),
        taskbar_widgets::generated::kWidgets.end(),
        [](const auto& widget) { return widget.id == L"system-cpu"; });
    assert(cpu != taskbar_widgets::generated::kWidgets.end());
    assert(cpu->width == 32.0 && cpu->height == 24.0);
    assert(taskbar_widgets::SystemMeterWidth(L"bar", 8) == 85.0);
    assert(taskbar_widgets::SystemMeterWidth(L"pie", 4) == 105.0);
    assert(taskbar_widgets::SystemMeterWidth(L"text", 2) == 91.0);

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
