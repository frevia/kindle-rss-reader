#include "device.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace rss {
void TouchGesture::update(TouchPoint point){
    end=point;minimum.x=std::min(minimum.x,point.x);minimum.y=std::min(minimum.y,point.y);
    maximum.x=std::max(maximum.x,point.x);maximum.y=std::max(maximum.y,point.y);
}
std::optional<Rect> changed_region(const std::vector<std::uint8_t>& before,
                                  const std::vector<std::uint8_t>& after){
    constexpr int width=1072,height=1448;
    if(before.size()!=width*height||after.size()!=before.size())throw std::runtime_error("屏幕像素数量不匹配");
    int left=width,top=height,right=-1,bottom=-1;
    for(int y=0;y<height;++y){
        const auto* previous=before.data()+y*width;
        const auto* current=after.data()+y*width;
        if(std::memcmp(previous,current,width)==0)continue;
        int first=0,last=width-1;
        while(previous[first]==current[first])++first;
        while(previous[last]==current[last])--last;
        left=std::min(left,first);right=std::max(right,last);top=std::min(top,y);bottom=y;
    }
    if(right<0)return std::nullopt;
    left=left/8*8;top=top/8*8;right=std::min(width,(right/8+1)*8);bottom=std::min(height,(bottom/8+1)*8);
    return Rect{left,top,right-left,bottom-top};
}
#ifdef __linux__
namespace {
// Linux Kindle classic MXCFB ABI; see docs/DEVICE.md for upstream references.
struct Update {
    std::uint32_t top,left,width,height,waveform,mode,marker,hist_bw,hist_gray;
    std::int32_t temperature;
    std::uint32_t flags,alternate[7];
};
struct Marker {std::uint32_t id,collision;};
struct InputEvent {std::int32_t sec,usec;std::uint16_t type,code;std::int32_t value;};
static_assert(sizeof(Update)==72&&sizeof(Marker)==8&&sizeof(InputEvent)==16,"Kindle kernel ABI");
[[noreturn]] void device_error(const char* operation){throw std::runtime_error(std::string(operation)+": "+std::strerror(errno));}
}
struct KindleDevice::Impl {
    int fb=-1,input=-1;std::uint8_t* memory=nullptr;FrameLayout layout;
    input_absinfo x{},y{};TouchDecoder decoder;
    std::vector<std::uint8_t> original,last;
    std::uint32_t marker=0,updates=0;
    ~Impl(){
        if(input>=0){ioctl(input,EVIOCGRAB,0);::close(input);}
        if(memory)::munmap(memory,layout.memory_size);
        if(fb>=0)::close(fb);
    }
    void check_layout(){
        fb_var_screeninfo v{};
        if(ioctl(fb,FBIOGET_VSCREENINFO,&v)<0)device_error("读取屏幕状态失败");
        if(v.xres!=layout.width||v.yres!=layout.height||v.bits_per_pixel!=8||v.rotate!=3||
           v.xoffset!=layout.xoffset||v.yoffset!=layout.yoffset)
            throw std::runtime_error("屏幕模式已改变，停止刷新");
    }
    void refresh(Rect r,bool full){
        if(++marker==0)++marker;
        Update update{};
        update.top=r.y;update.left=r.x;update.width=r.w;update.height=r.h;
        update.waveform=2;update.mode=full?1:0;update.marker=marker;
        update.hist_bw=1;update.hist_gray=2;update.temperature=0x1001;
        if(ioctl(fb,_IOW('F',0x2e,Update),&update)<0)device_error("提交墨水屏刷新失败");
        Marker wait{marker,0};
        if(ioctl(fb,_IOWR('F',0x2f,Marker),&wait)<0)device_error("等待墨水屏刷新失败");
    }
    void write(const std::vector<std::uint8_t>& pixels,Rect r){
        for(int y=r.y;y<r.y+r.h;++y)
            std::memcpy(memory+(layout.yoffset+y)*layout.stride+layout.xoffset+r.x,
                        pixels.data()+y*1072+r.x,r.w);
    }
};
KindleDevice::KindleDevice():impl_(std::make_unique<Impl>()){
    if(sizeof(void*)!=4)throw std::runtime_error("设备运行仅支持 ARM32 Kindle");
    auto& d=*impl_;
    d.fb=::open("/dev/fb0",O_RDWR|O_CLOEXEC);if(d.fb<0)device_error("打开屏幕失败");
    fb_var_screeninfo v{};fb_fix_screeninfo f{};
    if(ioctl(d.fb,FBIOGET_VSCREENINFO,&v)<0||ioctl(d.fb,FBIOGET_FSCREENINFO,&f)<0)device_error("读取屏幕参数失败");
    d.layout={v.xres,v.yres,f.line_length,v.bits_per_pixel,v.xoffset,v.yoffset,v.yres_virtual,f.smem_len};
    validate_kpw3_layout(d.layout);
    if(v.rotate!=3||std::string(f.id,strnlen(f.id,sizeof(f.id)))!="mxc_epdc_fb")
        throw std::runtime_error("屏幕驱动或方向不符合 KPW3 要求");
    for(int i=0;i<16;++i){
        const auto path="/dev/input/event"+std::to_string(i);
        const int fd=::open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)continue;
        char name[128]{};
        const bool matches=ioctl(fd,EVIOCGNAME(sizeof(name)),name)>=0&&std::string(name)=="cyttsp4_mt";
        if(matches){d.input=fd;break;}::close(fd);
    }
    if(d.input<0)throw std::runtime_error("未找到 KPW3 触摸设备");
    input_absinfo slot{};
    if(ioctl(d.input,EVIOCGABS(ABS_MT_POSITION_X),&d.x)<0||ioctl(d.input,EVIOCGABS(ABS_MT_POSITION_Y),&d.y)<0||
       ioctl(d.input,EVIOCGABS(ABS_MT_SLOT),&slot)<0)device_error("读取触摸范围失败");
    scale_touch({d.x.minimum,d.y.minimum},d.x.minimum,d.x.maximum,d.y.minimum,d.y.maximum);
    if(slot.minimum!=0)throw std::runtime_error("不支持的触摸槽范围");
    d.decoder.consume(EV_ABS,ABS_MT_SLOT,slot.value);
    void* mapped=::mmap(nullptr,d.layout.memory_size,PROT_READ|PROT_WRITE,MAP_SHARED,d.fb,0);
    if(mapped==MAP_FAILED)device_error("映射屏幕失败");d.memory=static_cast<std::uint8_t*>(mapped);
    d.original.resize(1072*1448);
    for(int y=0;y<1448;++y)std::memcpy(d.original.data()+y*1072,d.memory+(d.layout.yoffset+y)*d.layout.stride+d.layout.xoffset,1072);
    d.last=d.original;
    if(ioctl(d.input,EVIOCGRAB,1)<0)device_error("接管触摸失败");
}
KindleDevice::~KindleDevice()=default;
void KindleDevice::present(const std::vector<std::uint8_t>& pixels){
    auto& d=*impl_;d.check_layout();auto region=changed_region(d.last,pixels);if(!region)return;
    const bool full=d.updates%6==0;if(full)region=Rect{0,0,1072,1448};
    d.write(pixels,*region);d.refresh(*region,full);d.last=pixels;++d.updates;
}
void KindleDevice::restore(){
    auto& d=*impl_;d.check_layout();d.write(d.original,{0,0,1072,1448});d.refresh({0,0,1072,1448},true);d.last=d.original;
}
std::optional<TouchGesture> KindleDevice::touch(int timeout_ms){
    auto& d=*impl_;pollfd poller{d.input,POLLIN,0};
    const int ready=::poll(&poller,1,std::clamp(timeout_ms,0,1000));
    if(ready<0){if(errno==EINTR)return std::nullopt;device_error("等待触摸失败");}
    if(!ready)return std::nullopt;
    if(poller.revents&(POLLERR|POLLHUP|POLLNVAL))throw std::runtime_error("触摸设备已断开");
    InputEvent event{};
    for(int n=0;n<256;++n){
        const auto bytes=::read(d.input,&event,sizeof(event));
        if(bytes<0&&(errno==EAGAIN||errno==EINTR))return std::nullopt;
        if(bytes!=sizeof(event))throw std::runtime_error("触摸事件读取不完整");
        if(auto gesture=d.decoder.consume(event.type,event.code,event.value))
            return scale_gesture(*gesture,d.x.minimum,d.x.maximum,d.y.minimum,d.y.maximum);
    }
    return std::nullopt;
}
#else
struct KindleDevice::Impl {};
KindleDevice::KindleDevice(){throw std::runtime_error("设备运行仅在 Linux Kindle 上可用");}
KindleDevice::~KindleDevice()=default;
void KindleDevice::present(const std::vector<std::uint8_t>&){throw std::runtime_error("没有 Kindle 屏幕");}
void KindleDevice::restore(){}
std::optional<TouchGesture> KindleDevice::touch(int){return std::nullopt;}
#endif
void validate_kpw3_layout(const FrameLayout& l){
    if(l.width!=1072||l.height!=1448||l.bpp!=8||l.stride<l.width||l.stride>8192||
       l.xoffset>l.stride-l.width||l.virtual_height<l.height||l.yoffset>l.virtual_height-l.height||
       static_cast<std::uint64_t>(l.stride)*(l.yoffset+l.height)>l.memory_size||l.memory_size>64*1024*1024)
        throw std::runtime_error("屏幕布局不符合 KPW3 8 位竖屏要求");
}
std::optional<TouchGesture> TouchDecoder::consume(std::uint16_t type,std::uint16_t code,std::int32_t value){
    if(type==0&&code==3){
        active_=released_=have_x_=have_y_=path_started_=cancelled_=false;
        gesture_.reset();active_slots_.clear();slot_=x_=y_=0;
        throw std::runtime_error("触摸事件丢失，需要重新读取设备状态");
    }
    if(type==3){
        if(code==0x2f)slot_=value;
        if(slot_==0){
            if(code==0x35){x_=value;have_x_=true;}
            else if(code==0x36){y_=value;have_y_=true;}
            else if(code==0x39){
                if(value>=0){
                    active_=true;released_=false;path_started_=false;gesture_.reset();
                    cancelled_=!active_slots_.empty();
                }else{released_=active_;active_=false;}
            }
        }else if(code==0x39){
            if(value>=0){
                if(std::find(active_slots_.begin(),active_slots_.end(),slot_)==active_slots_.end())active_slots_.push_back(slot_);
                if(active_||released_)cancelled_=true;
            }else active_slots_.erase(std::remove(active_slots_.begin(),active_slots_.end(),slot_),active_slots_.end());
        }
    }
    if(type==0&&code==0){
        if(released_){
            released_=false;
            if(gesture_)gesture_->update({x_,y_});
            auto completed=(!cancelled_&&path_started_)?gesture_:std::nullopt;
            active_=path_started_=cancelled_=false;gesture_.reset();
            return completed;
        }
        if(active_&&have_x_&&have_y_){
            if(!path_started_){gesture_.emplace(TouchPoint{x_,y_});path_started_=true;}
            else gesture_->update({x_,y_});
        }
    }
    return std::nullopt;
}
TouchPoint scale_touch(TouchPoint p,int xmin,int xmax,int ymin,int ymax){
    if(xmin<0||ymin<0||xmax<=xmin||ymax<=ymin||xmax>65535||ymax>65535||
       p.x<xmin||p.x>xmax||p.y<ymin||p.y>ymax)throw std::runtime_error("无效触摸坐标范围");
    return {static_cast<int>((static_cast<std::int64_t>(p.x-xmin)*1071+(xmax-xmin)/2)/(xmax-xmin)),
            static_cast<int>((static_cast<std::int64_t>(p.y-ymin)*1447+(ymax-ymin)/2)/(ymax-ymin))};
}
TouchGesture scale_gesture(const TouchGesture& gesture,int xmin,int xmax,int ymin,int ymax){
    TouchGesture scaled(scale_touch(gesture.start,xmin,xmax,ymin,ymax));
    scaled.end=scale_touch(gesture.end,xmin,xmax,ymin,ymax);
    scaled.minimum=scale_touch(gesture.minimum,xmin,xmax,ymin,ymax);
    scaled.maximum=scale_touch(gesture.maximum,xmin,xmax,ymin,ymax);
    return scaled;
}
std::string probe_device(){
#ifdef __linux__
    struct FD{int fd;~FD(){if(fd>=0)::close(fd);}};
    FD fb{::open("/dev/fb0",O_RDONLY|O_CLOEXEC)};
    if(fb.fd<0)throw std::runtime_error("无法只读打开 /dev/fb0");
    fb_var_screeninfo v{};fb_fix_screeninfo f{};
    if(ioctl(fb.fd,FBIOGET_VSCREENINFO,&v)<0||ioctl(fb.fd,FBIOGET_FSCREENINFO,&f)<0)
        throw std::runtime_error("无法读取屏幕参数");
    FrameLayout layout{v.xres,v.yres,f.line_length,v.bits_per_pixel,v.xoffset,v.yoffset,v.yres_virtual,f.smem_len};
    std::ostringstream out;
    out<<"width="<<v.xres<<"\nheight="<<v.yres<<"\nstride="<<f.line_length<<"\nbpp="<<v.bits_per_pixel
       <<"\nxoffset="<<v.xoffset<<"\nyoffset="<<v.yoffset<<"\nvirtual_height="<<v.yres_virtual
       <<"\nmemory_size="<<f.smem_len<<"\nrotation="<<v.rotate<<"\ndriver="<<std::string(f.id,strnlen(f.id,sizeof(f.id)))<<"\n";
    validate_kpw3_layout(layout);out<<"layout=accepted\n";
    for(int i=0;i<16;++i){
        const auto path="/dev/input/event"+std::to_string(i);FD input{::open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC)};
        if(input.fd<0)continue;char name[128]{};
        if(ioctl(input.fd,EVIOCGNAME(sizeof(name)),name)<0)continue;
        out<<path<<"="<<name<<"\n";
        input_absinfo x{},y{};
        if(ioctl(input.fd,EVIOCGABS(ABS_MT_POSITION_X),&x)==0&&ioctl(input.fd,EVIOCGABS(ABS_MT_POSITION_Y),&y)==0)
            out<<"touch_x="<<x.minimum<<","<<x.maximum<<"\ntouch_y="<<y.minimum<<","<<y.maximum<<"\n";
    }
    return out.str();
#else
    throw std::runtime_error("设备诊断仅在 Linux Kindle 上可用");
#endif
}
}
