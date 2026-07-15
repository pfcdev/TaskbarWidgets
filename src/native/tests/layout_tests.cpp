#include "layout_math.h"

#include <cassert>

int main() {
    using taskbar_widgets::ClampHostWidth;
    using taskbar_widgets::LeftForWidget;
    using taskbar_widgets::PositionedWidget;

    assert(LeftForWidget(PositionedWidget{200, 100, 0}, 1000) == 800);
    assert(LeftForWidget(PositionedWidget{200, 50, -20}, 1000) == 280);
    assert(LeftForWidget(PositionedWidget{200, 200, 900}, 1000) == 1440);
    assert(ClampHostWidth(-10) == 1);
    assert(ClampHostWidth(5000) == 4096);
    return 0;
}
