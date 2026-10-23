#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include "render.hpp"

namespace rss {
struct FrameLayout {
    std::uint32_t width=0,height=0,stride=0,bpp=0,xoffset=0,yoffset=0,virtual_height=0;
    std::uint64_t memory_size=0;
};
void validate_kpw3_layout(const FrameLayout& layout);
struct TouchPoint { int x=0,y=0; };
class TouchDecoder {
public:
    std::optional<TouchPoint> consume(std::uint16_t type,std::uint16_t code,std::int32_t value);
private:
    int slot_=0,x_=0,y_=0;
    bool active_=false,released_=false,have_x_=false,have_y_=false;
};
TouchPoint scale_touch(TouchPoint point,int xmin,int xmax,int ymin,int ymax);
std::string probe_device(); // Read-only; does not grab input or change framebuffer.
std::optional<Rect> changed_region(const std::vector<std::uint8_t>& before,
                                  const std::vector<std::uint8_t>& after);
// The launcher must own the system UI session for this object's entire lifetime.
class KindleDevice {
public:
    KindleDevice();
    ~KindleDevice();
    KindleDevice(const KindleDevice&) = delete;
    KindleDevice& operator=(const KindleDevice&) = delete;
    void present(const std::vector<std::uint8_t>& pixels);
    std::optional<TouchPoint> touch(int timeout_ms);
    void restore(); // Only while the device is awake and the launcher still owns the UI.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
