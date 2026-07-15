#pragma once

#include <string_view>

namespace taskbar_widgets {

struct WidgetSize {
    double width;
    double height;
};

// Compile-time extension point. Renderers are linked into the Explorer hook;
// Taskbar Widgets never loads third-party DLLs into explorer.exe.
class IWidgetRenderer {
public:
    virtual ~IWidgetRenderer() = default;
    virtual std::wstring_view Id() const noexcept = 0;
    virtual WidgetSize DesiredSize() const noexcept = 0;
    virtual void Update(std::string_view snapshotJson) noexcept = 0;
    virtual bool HandleAction(std::wstring_view action) noexcept = 0;
};

}  // namespace taskbar_widgets
