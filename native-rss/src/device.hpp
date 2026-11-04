#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include "render.hpp"

namespace rss {
struct KindleProfile {
    const char* model="Unknown";
    int dpi=0;
    const char* display_platform="unknown";
    bool snow=false;
    const char* historical_firmware="none";
};
KindleProfile kindle_profile(const std::string& serial);
bool touch_tool_allowed(const std::string& name,bool direct,bool pointer,bool pen,bool finger);
struct FrameLayout {
    std::uint32_t width=0,height=0,stride=0,bpp=0,xoffset=0,yoffset=0,virtual_height=0;
    std::uint64_t memory_size=0;
};
void validate_kpw3_layout(const FrameLayout& layout);
void validate_frame_layout(const FrameLayout& layout);
struct TouchPoint { int x=0,y=0; };
struct ScreenTransform {
    int physical_width=0,physical_height=0,x=0,y=0,width=0,height=0;
    explicit ScreenTransform(int physical_width,int physical_height);
    std::vector<std::uint8_t> scale(const std::vector<std::uint8_t>& pixels,int logical_width=1072,int logical_height=1448) const;
    std::optional<TouchPoint> to_logical(TouchPoint point,int logical_width=1072,int logical_height=1448) const;
};
enum class TouchPhase { Complete, LongPress, Move, Cancel };
struct TouchGesture {
    TouchPoint start,end,minimum,maximum;
    TouchPhase phase=TouchPhase::Complete;
    std::uint64_t duration_ms=0,hold_ms=0;
    explicit TouchGesture(TouchPoint point):start(point),end(point),minimum(point),maximum(point){}
    void update(TouchPoint point);
};
TouchGesture map_touch_gesture(const TouchGesture& physical,const ScreenTransform& screen);
class TouchDecoder {
public:
    enum class Protocol { TypeA, TypeB, Single, Snow };
    explicit TouchDecoder(Protocol protocol=Protocol::TypeB,TouchPoint tolerance={24,24});
    std::optional<TouchGesture> consume(std::uint16_t type,std::uint16_t code,std::int32_t value,std::uint64_t now_ms=0);
    std::optional<TouchGesture> poll(std::uint64_t now_ms);
    Protocol protocol() const{return protocol_;}
private:
    Protocol protocol_;
    TouchPoint tolerance_;
    int slot_=0,x_=0,y_=0;
    struct Slot {int x=0,y=0;bool have_x=false,have_y=false;};
    std::array<Slot,32> slots_{};
    int primary_slot_=-1;
    bool active_=false,released_=false,have_x_=false,have_y_=false,path_started_=false,cancelled_=false;
    std::optional<TouchGesture> gesture_;
    std::vector<int> active_slots_;
    std::uint64_t start_ms_=0,hold_ms_=0,last_ms_=0;
    TouchPoint reported_{};
    bool have_time_=false,left_bounds_=false,long_sent_=false,cancel_sent_=false;
    int typea_contacts_=0,typea_x_=0,typea_y_=0;bool typea_have_x_=false,typea_have_y_=false,single_down_=false,typea_cancel_until_lift_=false;
    std::uint32_t snow_ids_=0;
};
bool kindle_snow_serial(const std::string& serial);
TouchDecoder::Protocol select_touch_protocol(bool has_slot,bool has_tracking_id,bool has_btn_touch,bool snow_model);
TouchPoint scale_touch(TouchPoint point,int xmin,int xmax,int ymin,int ymax,int width=1072,int height=1448);
TouchGesture scale_gesture(const TouchGesture& gesture,int xmin,int xmax,int ymin,int ymax,int width=1072,int height=1448);
std::string probe_device(); // Read-only; does not grab input or change framebuffer.
std::optional<Rect> changed_region(const std::vector<std::uint8_t>& before,
                                  const std::vector<std::uint8_t>& after,
                                  int width=1072,int height=1448);
std::vector<std::string> fbink_refresh_args(Rect region,bool full);
void refresh_fbink(Rect region,bool full,const std::string& executable="");
// The launcher must own the system UI session for this object's entire lifetime.
class KindleDevice {
public:
    KindleDevice();
    ~KindleDevice();
    KindleDevice(const KindleDevice&) = delete;
    KindleDevice& operator=(const KindleDevice&) = delete;
    void present(const std::vector<std::uint8_t>& pixels);
    std::optional<TouchGesture> touch(int timeout_ms);
    void restore(); // Only while the device is awake and the launcher still owns the UI.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
