#include "device.hpp"
#include "process.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <chrono>
#include <cstdio>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace rss {
KindleProfile kindle_profile(const std::string& serial){
    if(serial.size()<6||serial.size()>64)return {};
    for(unsigned char c:serial)if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')))return {};
    const bool legacy=serial[0]=='B'||serial[0]=='9';
    const auto code="|"+serial.substr(legacy?2:3,legacy?2:3)+"|";
    // Model hints never replace the framebuffer and evdev capability checks.
    static const struct {bool legacy;const char* codes;KindleProfile profile;} models[]={
        {true,"|0F|11|10|12|",{"Touch",167,"classic"}},
        {true,"|24|1B|1D|1F|1C|20|",{"Paperwhite 1",212,"classic"}},
        {true,"|D4|5A|D5|D6|D7|D8|F2|17|60|F4|F9|62|61|5F|",{"Paperwhite 2",212,"classic"}},
        {true,"|C6|DD|",{"Basic 2014",167,"classic"}},
        {true,"|13|54|2A|4F|52|53|",{"Voyage",300,"classic"}},
        {false,"|0G1|0G2|0G4|0G5|0G6|0G7|0KB|0KC|0KD|0KE|0KF|0KG|0LK|0LL|",{"Paperwhite 3",300,"classic",false,"5.16.2.1.1"}},
        {false,"|0GC|0GD|0GR|0GS|0GT|0GU|",{"Oasis 1",300,"classic"}},
        {false,"|0LM|0LN|0LP|0LQ|0P1|0P2|0P6|0P7|0P8|0S1|0S2|0S3|0S4|0S7|0SA|",{"Oasis 2",300,"zelda"}},
        {false,"|0DU|0K9|0KA|",{"Basic 2016",167,"classic"}},
        {false,"|0PP|0T1|0T2|0T3|0T4|0T5|0T6|0T7|0TJ|0TK|0TL|0TM|0TN|102|103|16Q|16R|16S|16T|16U|16V|",{"Paperwhite 4",300,"rex"}},
        {false,"|10L|0WF|0WG|0WH|0WJ|0VB|",{"Basic 2019",167,"rex",true}},
        {false,"|11L|0WQ|0WP|0WN|0WM|0WL|",{"Oasis 3",300,"zelda"}},
        {false,"|1Q0|1PX|1VD|21A|2BJ|2DK|",{"Paperwhite 5",300,"mtk"}},
        {false,"|1LG|219|2BH|",{"Paperwhite 5 SE",300,"mtk"}},
        {false,"|22D|25T|23A|2AQ|2AP|1XH|22C|",{"Basic 2022",300,"mtk"}},
        {false,"|27J|2BL|263|227|2BM|23L|23M|270|3V0|3V1|3X5|3UV|3X4|3X3|41E|410|",{"Scribe 1/2",300,"mtk"}},
        {false,"|3H2|3H4|3H6|3H7|3H9|3JT|3J6|455|456|4EP|34X|3HB|",{"Colorsoft",300,"mtk"}},
        {false,"|A89|3L2|3L3|3L4|3L5|3L6|3KM|",{"Basic 2024",300,"mtk"}},
        {false,"|33W|33X|346|349|3H3|3H5|3H8|3HA|3J5|3JS|",{"Paperwhite 6",300,"mtk"}},
        {false,"|4PG|4PE|4PL|4F8|4FA|454|",{"Scribe 3",300,"mtk"}},
        {false,"|4VX|4PF|4PH|4F9|4FB|46P|",{"Scribe Colorsoft",300,"mtk"}},
    };
    for(const auto& model:models)if(model.legacy==legacy&&std::strstr(model.codes,code.c_str()))return model.profile;
    return {};
}
bool kindle_snow_serial(const std::string& serial){return kindle_profile(serial).snow;}
TouchDecoder::Protocol select_touch_protocol(bool has_slot,bool has_tracking_id,bool has_btn_touch,bool snow_model){
    if(has_slot&&has_tracking_id)return TouchDecoder::Protocol::TypeB;
    if(!has_slot&&snow_model&&has_tracking_id&&has_btn_touch)return TouchDecoder::Protocol::Snow;
    return TouchDecoder::Protocol::TypeA;
}
std::vector<std::string> fbink_refresh_args(Rect r,bool full){
    if(r.x<0||r.y<0||r.w<=0||r.h<=0)throw std::runtime_error("无效刷新区域");
    std::vector<std::string> args{"/mnt/us/libkh/bin/fbink","-q","-w","-W","GC16"};if(full)args.push_back("-f");
    args.insert(args.end(),{"--refresh","top="+std::to_string(r.y)+",left="+std::to_string(r.x)+",width="+std::to_string(r.w)+",height="+std::to_string(r.h)});return args;
}
void refresh_fbink(Rect region,bool full,const std::string& executable){
    auto args=fbink_refresh_args(region,full);args[0]=executable;
    const auto output=capture_bounded(args,std::chrono::seconds(5),4096,true);
    // FBInk's CLI ignores wait errors; WARN remains visible with -q. Only this optional wait is harmless.
    const std::string optional_wait="[FBInk] Waiting for update submission is not supported on your device!";
    if(!output.empty()&&output!=optional_wait&&output!=optional_wait+"\n")
        throw std::runtime_error("FBInk 刷新未确认完成："+output);
}
void TouchGesture::update(TouchPoint point){
    end=point;minimum.x=std::min(minimum.x,point.x);minimum.y=std::min(minimum.y,point.y);
    maximum.x=std::max(maximum.x,point.x);maximum.y=std::max(maximum.y,point.y);
}
std::optional<Rect> changed_region(const std::vector<std::uint8_t>& before,
                                  const std::vector<std::uint8_t>& after,int width,int height){
    if(width<=0||height<=0||static_cast<std::uint64_t>(width)*height>64*1024*1024||before.size()!=static_cast<std::size_t>(width)*height||after.size()!=before.size())throw std::runtime_error("屏幕像素数量不匹配");
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
static_assert(sizeof(void*)!=4||sizeof(input_event)==16,"Kindle input event ABI");
[[noreturn]] void device_error(const char* operation){throw std::runtime_error(std::string(operation)+": "+std::strerror(errno));}
struct InputInfo{std::string path,name;TouchDecoder::Protocol protocol;input_absinfo x{},y{};};
bool bit(const unsigned long* bits,int n){return bits[n/(sizeof(unsigned long)*8)]&(1UL<<(n%(sizeof(unsigned long)*8)));}
KindleProfile read_profile(){
    std::ifstream usid("/proc/usid",std::ios::binary);char serial[64]{};usid.read(serial,sizeof(serial));
    auto size=static_cast<std::size_t>(usid.gcount());if(size==sizeof(serial))return {};
    while(size&&(serial[size-1]=='\n'||serial[size-1]=='\r'))--size;
    return kindle_profile(std::string(serial,size));
}
const char* protocol_name(TouchDecoder::Protocol protocol){
    switch(protocol){case TouchDecoder::Protocol::TypeA:return "type-a";case TouchDecoder::Protocol::TypeB:return "type-b";case TouchDecoder::Protocol::Single:return "single";case TouchDecoder::Protocol::Snow:return "snow";}
    return "unknown";
}
std::optional<InputInfo> inspect_input(const std::string& path,bool snow_model){
    int fd=::open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0)return std::nullopt;
    InputInfo info{};info.path=path;char name[128]{};if(ioctl(fd,EVIOCGNAME(sizeof(name)),name)<0){::close(fd);return std::nullopt;}info.name=name;
    unsigned long props[(INPUT_PROP_MAX+sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)]{};
    unsigned long keys[(KEY_MAX+sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)]{};
    unsigned long abs[(ABS_MAX+sizeof(unsigned long)*8)/(sizeof(unsigned long)*8)]{};
    ioctl(fd,EVIOCGPROP(sizeof(props)),props);ioctl(fd,EVIOCGBIT(EV_KEY,sizeof(keys)),keys);ioctl(fd,EVIOCGBIT(EV_ABS,sizeof(abs)),abs);
    const bool mt=bit(abs,ABS_MT_POSITION_X)&&bit(abs,ABS_MT_POSITION_Y);
    const bool direct=bit(props,INPUT_PROP_DIRECT);
    const bool excluded=bit(keys,BTN_TOOL_PEN)||bit(props,INPUT_PROP_POINTER)||(!direct&&bit(keys,BTN_TOOL_FINGER));
    const bool touchscreen=direct||bit(keys,BTN_TOUCH);
    bool ok=false;
    if(touchscreen&&!excluded&&mt&&ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&info.x)==0&&ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&info.y)==0&&info.x.maximum>info.x.minimum&&info.y.maximum>info.y.minimum){
        const bool has_slot=bit(abs,ABS_MT_SLOT),has_tracking_id=bit(abs,ABS_MT_TRACKING_ID),has_btn_touch=bit(keys,BTN_TOUCH);
        info.protocol=select_touch_protocol(has_slot,has_tracking_id,has_btn_touch,snow_model);
        if(info.protocol==TouchDecoder::Protocol::TypeB||info.protocol==TouchDecoder::Protocol::Snow)ok=true;
        else if(!has_slot)ok=true;
    }else if(touchscreen&&!excluded&&bit(keys,BTN_TOUCH)&&bit(abs,ABS_X)&&bit(abs,ABS_Y)&&ioctl(fd,EVIOCGABS(ABS_X),&info.x)==0&&ioctl(fd,EVIOCGABS(ABS_Y),&info.y)==0&&info.x.maximum>info.x.minimum&&info.y.maximum>info.y.minimum){info.protocol=TouchDecoder::Protocol::Single;ok=true;}
    if(ok&&info.protocol==TouchDecoder::Protocol::TypeB){
        input_absinfo slots{};
        ok=ioctl(fd,EVIOCGABS(ABS_MT_SLOT),&slots)==0&&slots.minimum==0&&slots.maximum>=0&&slots.maximum<=31;
    }
    ::close(fd);return ok?std::optional<InputInfo>(info):std::nullopt;
}
}
struct KindleDevice::Impl {
    int fb=-1,input=-1;std::uint8_t* memory=nullptr;FrameLayout layout;
    input_absinfo x{},y{};TouchDecoder::Protocol protocol=TouchDecoder::Protocol::TypeB;TouchDecoder decoder;ScreenTransform transform{1072,1448};
    std::vector<std::uint8_t> original,last;
    std::uint32_t updates=0,rotation=0,grayscale=0;std::string driver;bool presented=false;
    std::uint64_t frames=0,skipped=0,scale_us=0,diff_us=0,copy_us=0,fbink_us=0;
    ~Impl(){
        if(frames)std::fprintf(stderr,"display_metrics frames=%llu skipped=%llu refreshes=%u scale_us=%llu diff_us=%llu copy_us=%llu fbink_us=%llu\n",
            static_cast<unsigned long long>(frames),static_cast<unsigned long long>(skipped),updates,
            static_cast<unsigned long long>(scale_us),static_cast<unsigned long long>(diff_us),
            static_cast<unsigned long long>(copy_us),static_cast<unsigned long long>(fbink_us));
        if(input>=0){ioctl(input,EVIOCGRAB,0);::close(input);}
        if(memory)::munmap(memory,layout.memory_size);
        if(fb>=0)::close(fb);
    }
    void check_layout(){
        fb_var_screeninfo v{};
        if(ioctl(fb,FBIOGET_VSCREENINFO,&v)<0)device_error("读取屏幕状态失败");
        fb_fix_screeninfo f{};if(ioctl(fb,FBIOGET_FSCREENINFO,&f)<0)device_error("读取屏幕驱动失败");
        if(v.xres!=layout.width||v.yres!=layout.height||v.bits_per_pixel!=layout.bpp||v.rotate!=rotation||v.grayscale!=grayscale||
           v.xoffset!=layout.xoffset||v.yoffset!=layout.yoffset||v.yres_virtual!=layout.virtual_height||
           f.line_length!=layout.stride||f.smem_len!=layout.memory_size||std::string(f.id,strnlen(f.id,sizeof(f.id)))!=driver)
            throw std::runtime_error("屏幕模式已改变，停止刷新");
    }
    void refresh(Rect r,bool full){
        refresh_fbink(r,full);
    }
    void write(const std::vector<std::uint8_t>& pixels,Rect r){
        for(int y=r.y;y<r.y+r.h;++y)
            std::memcpy(memory+(layout.yoffset+y)*layout.stride+layout.xoffset+r.x,
                        pixels.data()+static_cast<std::size_t>(y)*layout.width+r.x,r.w);
    }
};
KindleDevice::KindleDevice():impl_(std::make_unique<Impl>()){
    if(sizeof(void*)!=4)throw std::runtime_error("设备运行仅支持 ARM32 Kindle");
    auto& d=*impl_;
    d.fb=::open("/dev/fb0",O_RDWR|O_CLOEXEC);if(d.fb<0)device_error("打开屏幕失败");
    fb_var_screeninfo v{};fb_fix_screeninfo f{};
    if(ioctl(d.fb,FBIOGET_VSCREENINFO,&v)<0||ioctl(d.fb,FBIOGET_FSCREENINFO,&f)<0)device_error("读取屏幕参数失败");
    d.layout={v.xres,v.yres,f.line_length,v.bits_per_pixel,v.xoffset,v.yoffset,v.yres_virtual,f.smem_len};
    validate_frame_layout(d.layout);if(v.rotate>3||v.grayscale>1)throw std::runtime_error("屏幕方向或灰度模式无效");
    d.rotation=v.rotate;d.grayscale=v.grayscale;d.driver=std::string(f.id,strnlen(f.id,sizeof(f.id)));d.transform=ScreenTransform(v.xres,v.yres);
    DIR* dir=::opendir("/dev/input");if(!dir)device_error("扫描触摸设备失败");std::vector<InputInfo> candidates;
    const bool snow_model=read_profile().snow;
    while(auto* entry=::readdir(dir)){const std::string name=entry->d_name;if(name.rfind("event",0)!=0)continue;auto info=inspect_input("/dev/input/"+name,snow_model);if(info)candidates.push_back(*info);}::closedir(dir);
    if(candidates.size()!=1)throw std::runtime_error(candidates.empty()?"未找到受支持的直接触摸设备":"存在多个触摸设备，拒绝不确定选择");
    auto chosen=candidates.front();d.input=::open(chosen.path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(d.input<0)device_error("打开触摸设备失败");
    d.x=chosen.x;d.y=chosen.y;d.protocol=chosen.protocol;
    d.decoder=TouchDecoder(d.protocol,{std::max(1,24*(d.transform.width-1)/1071),std::max(1,24*(d.transform.height-1)/1447)});
    scale_touch({d.x.minimum,d.y.minimum},d.x.minimum,d.x.maximum,d.y.minimum,d.y.maximum);
    if(d.protocol==TouchDecoder::Protocol::TypeB){input_absinfo slot{};if(ioctl(d.input,EVIOCGABS(ABS_MT_SLOT),&slot)<0||slot.minimum!=0)throw std::runtime_error("不支持的触摸槽范围");d.decoder.consume(EV_ABS,ABS_MT_SLOT,slot.value);}
    void* mapped=::mmap(nullptr,d.layout.memory_size,PROT_READ|PROT_WRITE,MAP_SHARED,d.fb,0);
    if(mapped==MAP_FAILED)device_error("映射屏幕失败");d.memory=static_cast<std::uint8_t*>(mapped);
    d.original.resize(static_cast<std::size_t>(d.layout.width)*d.layout.height);
    for(std::uint32_t y=0;y<d.layout.height;++y)std::memcpy(d.original.data()+static_cast<std::size_t>(y)*d.layout.width,d.memory+(d.layout.yoffset+y)*d.layout.stride+d.layout.xoffset,d.layout.width);
    d.last.assign(d.original.size(),255);
    if(ioctl(d.input,EVIOCGRAB,1)<0)device_error("接管触摸失败");
}
KindleDevice::~KindleDevice()=default;
void KindleDevice::present(const std::vector<std::uint8_t>& pixels){
    auto& d=*impl_;d.check_layout();const Rect screen{0,0,d.transform.physical_width,d.transform.physical_height};
    auto tick=std::chrono::steady_clock::now();
    auto elapsed=[&](){const auto now=std::chrono::steady_clock::now();const auto us=std::chrono::duration_cast<std::chrono::microseconds>(now-tick).count();tick=now;return us;};
    auto physical=d.transform.scale(pixels);++d.frames;d.scale_us+=elapsed();
    auto region=changed_region(d.last,physical,d.layout.width,d.layout.height);d.diff_us+=elapsed();
    if(!region&&!d.presented)region=screen;if(!region){++d.skipped;return;}
    const bool full=d.updates%6==0;if(full)region=screen;
    d.write(physical,*region);d.copy_us+=elapsed();d.refresh(*region,full);d.fbink_us+=elapsed();
    d.last=std::move(physical);d.presented=true;++d.updates;
}
void KindleDevice::restore(){
    auto& d=*impl_;d.check_layout();const Rect screen{0,0,d.transform.physical_width,d.transform.physical_height};
    d.write(d.original,screen);d.refresh(screen,true);d.last=d.original;
}
std::optional<TouchGesture> KindleDevice::touch(int timeout_ms){
    auto& d=*impl_;d.check_layout();pollfd poller{d.input,POLLIN,0};
    const int ready=::poll(&poller,1,std::clamp(timeout_ms,0,1000));
    if(ready<0&&errno!=EINTR)device_error("等待触摸失败");
    if(poller.revents&(POLLERR|POLLHUP|POLLNVAL))throw std::runtime_error("触摸设备已断开");
    auto now_ms=[](){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());};
    auto scale=[&](const TouchGesture& gesture)->std::optional<TouchGesture>{
        return map_touch_gesture(gesture,d.transform);
    };
    if(ready<=0){if(auto gesture=d.decoder.poll(now_ms()))return scale(*gesture);return std::nullopt;}
    input_event event{};
    std::optional<TouchGesture> latest_move;
    for(int n=0;n<256;++n){
        const auto bytes=::read(d.input,&event,sizeof(event));
        if(bytes<0&&(errno==EAGAIN||errno==EINTR)){
            if(auto gesture=d.decoder.poll(now_ms()))return scale(*gesture);
            return latest_move?scale(*latest_move):std::nullopt;
        }
        if(bytes!=sizeof(event))throw std::runtime_error("触摸事件读取不完整");
        // Normalize before gesture decoding so controller units cannot change hold sensitivity.
        if(event.type==EV_ABS){
            const bool single=d.protocol==TouchDecoder::Protocol::Single;
            if(event.code==(single?ABS_X:ABS_MT_POSITION_X))
                event.value=scale_touch({event.value,d.y.minimum},d.x.minimum,d.x.maximum,d.y.minimum,d.y.maximum,d.layout.width,d.layout.height).x;
            else if(event.code==(single?ABS_Y:ABS_MT_POSITION_Y))
                event.value=scale_touch({d.x.minimum,event.value},d.x.minimum,d.x.maximum,d.y.minimum,d.y.maximum,d.layout.width,d.layout.height).y;
        }
        if(auto gesture=d.decoder.consume(event.type,event.code,event.value,now_ms())){
            if(gesture->phase==TouchPhase::Move)latest_move=std::move(gesture);
            else return scale(*gesture);
        }
    }
    if(auto gesture=d.decoder.poll(now_ms()))return scale(*gesture);
    return latest_move?scale(*latest_move):std::nullopt;
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
    validate_frame_layout(l);
    if(l.width!=1072||l.height!=1448)throw std::runtime_error("屏幕布局不符合 KPW3 尺寸要求");
}
void validate_frame_layout(const FrameLayout& l){
    if(l.width<600||l.width>2560||l.height<800||l.height>3400||l.width>l.height||l.bpp!=8||
       l.stride<l.width||l.stride>8192||l.xoffset>l.stride-l.width||l.virtual_height<l.height||
       l.yoffset>l.virtual_height-l.height||l.memory_size>64*1024*1024||
       static_cast<std::uint64_t>(l.stride)*(static_cast<std::uint64_t>(l.yoffset)+l.height)>l.memory_size)
        throw std::runtime_error("屏幕布局不符合有界 8 位竖屏要求");
}
ScreenTransform::ScreenTransform(int pw,int ph):physical_width(pw),physical_height(ph){
    if(pw<600||pw>2560||ph<800||ph>3400||pw>ph)throw std::runtime_error("无效物理屏幕尺寸");
    // shortcut: scale the existing UI, add native-density layouts when individual devices are calibrated.
    if(static_cast<std::int64_t>(pw)*1448<=static_cast<std::int64_t>(ph)*1072){width=pw;height=static_cast<int>(static_cast<std::int64_t>(pw)*1448/1072);}
    else{height=ph;width=static_cast<int>(static_cast<std::int64_t>(ph)*1072/1448);}
    x=(pw-width)/2;y=(ph-height)/2;
}
std::vector<std::uint8_t> ScreenTransform::scale(const std::vector<std::uint8_t>& p,int lw,int lh) const{
    if(physical_width<600||physical_width>2560||physical_height<800||physical_height>3400||x<0||y<0||width<=1||height<=1||width>physical_width||height>physical_height||x>physical_width-width||y>physical_height-height||
       lw<=0||lh<=0||lw>2560||lh>3400||static_cast<std::uint64_t>(lw)*lh>64*1024*1024||p.size()!=static_cast<std::size_t>(lw)*lh||static_cast<std::uint64_t>(physical_width)*physical_height>64*1024*1024)
        throw std::runtime_error("缩放像素尺寸无效");
    if(width==lw&&height==lh&&x==0&&y==0&&physical_width==lw&&physical_height==lh)return p;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(physical_width)*physical_height,255);
    // Integrate pixel coverage so a one-pixel rule cannot disappear on a smaller display.
    for(int dy=0;dy<height;++dy)for(int dx=0;dx<width;++dx){
        const int x0=dx*lw,x1=(dx+1)*lw,y0=dy*lh,y1=(dy+1)*lh;
        std::int64_t sum=0;
        for(int sy=y0/height;sy<(y1+height-1)/height;++sy){
            const int wy=std::min(y1,(sy+1)*height)-std::max(y0,sy*height);
            for(int sx=x0/width;sx<(x1+width-1)/width;++sx){
                const int wx=std::min(x1,(sx+1)*width)-std::max(x0,sx*width);
                sum+=static_cast<std::int64_t>(p[static_cast<std::size_t>(sy)*lw+sx])*wx*wy;
            }
        }
        out[static_cast<std::size_t>(dy+y)*physical_width+dx+x]=static_cast<std::uint8_t>((sum+static_cast<std::int64_t>(lw)*lh/2)/(static_cast<std::int64_t>(lw)*lh));
    }
    return out;
}
std::optional<TouchPoint> ScreenTransform::to_logical(TouchPoint p,int lw,int lh) const{
    if(lw<=0||lh<=0||lw>2560||lh>3400||physical_width<600||physical_width>2560||physical_height<800||physical_height>3400||x<0||y<0||width<=1||height<=1||width>physical_width||height>physical_height||x>physical_width-width||y>physical_height-height||p.x<x||p.x>=x+width||p.y<y||p.y>=y+height)return std::nullopt;
    return TouchPoint{static_cast<int>(static_cast<std::int64_t>(p.x-x)*(lw-1)/(width-1)),static_cast<int>(static_cast<std::int64_t>(p.y-y)*(lh-1)/(height-1))};
}
TouchGesture map_touch_gesture(const TouchGesture& physical,const ScreenTransform& screen){
    auto mapped=physical;
    const auto start=screen.to_logical(physical.start),end=screen.to_logical(physical.end),
        minimum=screen.to_logical(physical.minimum),maximum=screen.to_logical(physical.maximum);
    if(!start||!end||!minimum||!maximum){mapped.phase=TouchPhase::Cancel;return mapped;}
    mapped.start=*start;mapped.end=*end;mapped.minimum=*minimum;mapped.maximum=*maximum;
    return mapped;
}
TouchDecoder::TouchDecoder(Protocol protocol,TouchPoint tolerance):protocol_(protocol),tolerance_(tolerance){
    if(tolerance.x<1||tolerance.y<1||tolerance.x>2560||tolerance.y>3400)throw std::runtime_error("无效触摸抖动范围");
}
std::optional<TouchGesture> TouchDecoder::consume(std::uint16_t type,std::uint16_t code,std::int32_t value,std::uint64_t now_ms){
    if(type==1&&code==330&&value==0){
        // BTN_TOUCH describes all contacts, regardless of the most recently selected MT slot.
        active_slots_.clear();released_=released_||active_;active_=false;
        if(!released_)primary_slot_=-1;
        typea_contacts_=0;typea_have_x_=typea_have_y_=single_down_=false;
        snow_ids_=0;
        return std::nullopt;
    }
    if(protocol_==Protocol::Single&&type==1&&(code==328||(code>=333&&code<=335))&&value){
        if(active_slots_.empty())active_slots_.push_back(1);
        if(active_)cancelled_=true;
        return std::nullopt;
    }
    if(protocol_==Protocol::TypeA&&!(type==0&&code==3)){
        if(type==3&&code==0x35){typea_x_=value;typea_have_x_=true;return std::nullopt;}
        if(type==3&&code==0x36){typea_y_=value;typea_have_y_=true;return std::nullopt;}
        if(type==0&&code==2){
            if(typea_have_x_!=typea_have_y_)throw std::runtime_error("Type A 触摸数据不完整");
            if(typea_have_x_&&typea_have_y_)typea_contacts_=std::min(2,typea_contacts_+1);
            typea_have_x_=typea_have_y_=false;return std::nullopt;
        }
        if(type==0&&code==0){
            if(typea_have_x_!=typea_have_y_)throw std::runtime_error("Type A 触摸数据不完整");
            if(typea_have_x_&&typea_have_y_)typea_contacts_=std::min(2,typea_contacts_+1);
            const int contacts=typea_contacts_;typea_contacts_=0;typea_have_x_=typea_have_y_=false;
            if(contacts==0){typea_cancel_until_lift_=false;if(active_||released_){const auto old=protocol_;protocol_=Protocol::TypeB;if(active_)consume(3,0x39,-1,now_ms);auto result=consume(0,0,0,now_ms);protocol_=old;return result;}return std::nullopt;}
            if(typea_cancel_until_lift_)return std::nullopt;
            const auto old=protocol_;protocol_=Protocol::TypeB;
            if(!active_)consume(3,0x39,0,now_ms);
            if(contacts>1){cancelled_=true;typea_cancel_until_lift_=true;}
            consume(3,0x35,typea_x_,now_ms);consume(3,0x36,typea_y_,now_ms);
            auto result=consume(0,0,0,now_ms);protocol_=old;return result;
        }
        return std::nullopt;
    }
    if(protocol_==Protocol::Single){
        if(type==3&&code==0)code=0x35;
        if(type==3&&code==1)code=0x36;
        if(type==1&&code==330){single_down_=value!=0;return consume(3,0x39,single_down_?0:-1,now_ms);}
    }
    if(have_time_&&now_ms<last_ms_)now_ms=last_ms_;
    last_ms_=now_ms;have_time_=true;
    if(type==0&&code==3){
        active_=released_=have_x_=have_y_=path_started_=cancelled_=false;
        gesture_.reset();active_slots_.clear();slot_=x_=y_=0;
        primary_slot_=-1;slots_={};
        typea_contacts_=0;typea_have_x_=typea_have_y_=single_down_=typea_cancel_until_lift_=false;
        snow_ids_=0;
        start_ms_=hold_ms_=last_ms_=0;have_time_=left_bounds_=long_sent_=cancel_sent_=false;
        throw std::runtime_error("触摸事件丢失，需要重新读取设备状态");
    }
    if(protocol_==Protocol::Snow){
        if(type==3&&code==0x2f){
            if(active_||released_||snow_ids_)throw std::runtime_error("活动触摸期间协议切换不安全");
            protocol_=Protocol::TypeB;snow_ids_=0;
        }
        if(type==3&&code==0x39){
            if(value==-1){
                protocol_=Protocol::TypeB;snow_ids_=0;
            }else{
                if(value<0||value>31)throw std::runtime_error("无效 Snow 触摸编号");
                if(released_)throw std::runtime_error("Snow 抬手帧尚未完成");
                slot_=value;
                const auto mask=std::uint32_t{1}<<value;
                if(snow_ids_&mask)return std::nullopt;
                snow_ids_|=mask;
            }
        }
    }
    if(type==3){
        if(code==0x2f){if(value<0||value>31)throw std::runtime_error("无效触摸槽编号");slot_=value;}
        auto& position=slots_[slot_];
        if(code==0x35){position.x=value;position.have_x=true;}
        if(code==0x36){position.y=value;position.have_y=true;}
        if(code==0x39&&value>=0&&primary_slot_<0){
            primary_slot_=slot_;x_=position.x;y_=position.y;have_x_=position.have_x;have_y_=position.have_y;
        }
        if(slot_==primary_slot_){
            if(code==0x35){x_=value;have_x_=true;}
            else if(code==0x36){y_=value;have_y_=true;}
            else if(code==0x39){
                if(value>=0){
                    active_=true;released_=false;path_started_=false;gesture_.reset();
                    left_bounds_=long_sent_=cancel_sent_=false;hold_ms_=0;
                    cancelled_=!active_slots_.empty();
                }else{released_=active_;active_=false;}
            }
        }else if(code==0x39){
            if(value>=0){
                if(std::find(active_slots_.begin(),active_slots_.end(),slot_)==active_slots_.end())active_slots_.push_back(slot_);
                if(active_||released_)cancelled_=true;
            }else{
                active_slots_.erase(std::remove(active_slots_.begin(),active_slots_.end(),slot_),active_slots_.end());
                if(!active_&&!released_&&active_slots_.empty())primary_slot_=-1;
            }
        }
    }
    if(type==0&&code==0){
        auto update=[&](TouchPoint point){
            gesture_->update(point);
            const auto dx=static_cast<std::int64_t>(point.x)-gesture_->start.x;
            const auto dy=static_cast<std::int64_t>(point.y)-gesture_->start.y;
            if(!left_bounds_&&(dx>tolerance_.x||dx< -tolerance_.x||dy>tolerance_.y||dy< -tolerance_.y)){
                left_bounds_=true;hold_ms_=now_ms>=start_ms_?now_ms-start_ms_:0;
            }
            gesture_->duration_ms=now_ms>=start_ms_?now_ms-start_ms_:0;
            gesture_->hold_ms=left_bounds_?hold_ms_:gesture_->duration_ms;
        };
        if(released_){
            if(gesture_)update({x_,y_});
            if(cancelled_&&long_sent_&&!cancel_sent_&&gesture_){
                auto cancelled=*gesture_;cancelled.phase=TouchPhase::Cancel;cancel_sent_=true;
                released_=false;active_=path_started_=cancelled_=false;gesture_.reset();long_sent_=false;
                if(active_slots_.empty())primary_slot_=-1;
                return cancelled;
            }
            released_=false;
            auto completed=(!cancelled_&&path_started_)?gesture_:std::nullopt;
            if(completed)completed->phase=TouchPhase::Complete;
            active_=path_started_=cancelled_=false;gesture_.reset();
            if(active_slots_.empty())primary_slot_=-1;
            return completed;
        }
        if(active_&&have_x_&&have_y_){
            if(!path_started_){
                gesture_.emplace(TouchPoint{x_,y_});path_started_=true;start_ms_=now_ms;
                hold_ms_=0;left_bounds_=long_sent_=cancel_sent_=false;reported_={x_,y_};
            }else update({x_,y_});
            if(cancelled_&&long_sent_&&!cancel_sent_&&gesture_){
                auto cancelled=*gesture_;cancelled.phase=TouchPhase::Cancel;cancel_sent_=true;
                return cancelled;
            }
            if(long_sent_&&!cancelled_&&gesture_&&(x_!=reported_.x||y_!=reported_.y)){
                reported_={x_,y_};auto moved=*gesture_;moved.phase=TouchPhase::Move;return moved;
            }
        }
    }
    return std::nullopt;
}
std::optional<TouchGesture> TouchDecoder::poll(std::uint64_t now_ms){
    if(have_time_&&now_ms<last_ms_)now_ms=last_ms_;
    last_ms_=now_ms;have_time_=true;
    if(!active_||!path_started_||cancelled_||left_bounds_||long_sent_||!gesture_)return std::nullopt;
    gesture_->duration_ms=now_ms>=start_ms_?now_ms-start_ms_:0;
    gesture_->hold_ms=gesture_->duration_ms;
    if(gesture_->duration_ms<600)return std::nullopt;
    long_sent_=true;reported_={x_,y_};gesture_->phase=TouchPhase::LongPress;return gesture_;
}
TouchPoint scale_touch(TouchPoint p,int xmin,int xmax,int ymin,int ymax,int width,int height){
    const auto xr=static_cast<std::int64_t>(xmax)-xmin,yr=static_cast<std::int64_t>(ymax)-ymin;
    if(xr<=0||yr<=0||p.x<xmin||p.x>xmax||p.y<ymin||p.y>ymax||width<1||width>2560||height<1||height>3400)throw std::runtime_error("无效触摸坐标范围");
    return {static_cast<int>(((static_cast<std::int64_t>(p.x)-xmin)*(width-1)+xr/2)/xr),
            static_cast<int>(((static_cast<std::int64_t>(p.y)-ymin)*(height-1)+yr/2)/yr)};
}
TouchGesture scale_gesture(const TouchGesture& gesture,int xmin,int xmax,int ymin,int ymax,int width,int height){
    TouchGesture scaled(scale_touch(gesture.start,xmin,xmax,ymin,ymax,width,height));
    scaled.end=scale_touch(gesture.end,xmin,xmax,ymin,ymax,width,height);
    scaled.minimum=scale_touch(gesture.minimum,xmin,xmax,ymin,ymax,width,height);
    scaled.maximum=scale_touch(gesture.maximum,xmin,xmax,ymin,ymax,width,height);
    scaled.phase=gesture.phase;scaled.duration_ms=gesture.duration_ms;scaled.hold_ms=gesture.hold_ms;
    return scaled;
}
std::string probe_device(){
#ifdef __linux__
    std::ostringstream out;
    const auto profile=read_profile();
    out<<"model="<<profile.model<<"\ndisplay_dpi="<<profile.dpi<<"\ndisplay_platform_hint="<<profile.display_platform
       <<"\ntouch_policy="<<(profile.snow?"snow-if-no-slots":"capabilities")<<"\nhistorical_firmware="<<profile.historical_firmware
       <<"\ncurrent_build_hardware_verified=0\n";
    int fb=::open("/dev/fb0",O_RDONLY|O_CLOEXEC);fb_var_screeninfo v{};fb_fix_screeninfo f{};FrameLayout layout{};bool have_layout=false;
    if(fb>=0&&ioctl(fb,FBIOGET_VSCREENINFO,&v)==0&&ioctl(fb,FBIOGET_FSCREENINFO,&f)==0){layout={v.xres,v.yres,f.line_length,v.bits_per_pixel,v.xoffset,v.yoffset,v.yres_virtual,f.smem_len};have_layout=true;}
    if(fb>=0)::close(fb);
    const auto driver=have_layout?std::string(f.id,strnlen(f.id,sizeof(f.id))):std::string();
    out<<"width="<<layout.width<<"\nheight="<<layout.height<<"\nstride="<<layout.stride<<"\nbpp="<<layout.bpp
       <<"\nxoffset="<<layout.xoffset<<"\nyoffset="<<layout.yoffset<<"\nvirtual_height="<<layout.virtual_height
       <<"\nmemory_size="<<layout.memory_size<<"\nrotation="<<(have_layout?v.rotate:0)<<"\ngrayscale="<<(have_layout?v.grayscale:0)<<"\ndriver="<<driver<<"\n"
       <<"logical_width=1072\nlogical_height=1448\nrefresh=fbink\n";
    bool layout_ok=false;std::string layout_reason="无法读取 framebuffer 参数";
    if(have_layout){try{validate_frame_layout(layout);if(v.rotate>3||v.grayscale>1)throw std::runtime_error("rotation 或 grayscale 不受支持");layout_ok=true;layout_reason.clear();}catch(const std::exception& e){layout_reason=e.what();}}
    const std::uint64_t snapshot=layout_ok?static_cast<std::uint64_t>(layout.stride)*(static_cast<std::uint64_t>(layout.yoffset)+layout.height):0;
    out<<"snapshot_bytes="<<snapshot<<"\nlayout="<<(layout_ok?"accepted":"rejected")<<"\n";
    if(!layout_ok)out<<"layout_reason="<<layout_reason<<"\n";
    std::vector<InputInfo> candidates;DIR* dir=::opendir("/dev/input");
    if(dir){const bool snow_model=profile.snow;while(auto* entry=::readdir(dir)){const std::string name=entry->d_name;if(name.rfind("event",0)!=0)continue;const auto path="/dev/input/"+name;
            int fd=::open(path.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
            if(fd>=0){
                char device_name[128]{};
                if(ioctl(fd,EVIOCGNAME(sizeof(device_name)),device_name)>=0){
                    input_absinfo a{},b{};
                    bool axes=ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&a)==0&&ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&b)==0;
                    if(!axes)axes=ioctl(fd,EVIOCGABS(ABS_X),&a)==0&&ioctl(fd,EVIOCGABS(ABS_Y),&b)==0;
                    out<<"input_device="<<device_name<<"\n";
                    if(axes)out<<"input_x_min="<<a.minimum<<"\ninput_x_max="<<a.maximum<<"\ninput_y_min="<<b.minimum<<"\ninput_y_max="<<b.maximum<<"\n";
                }
                ::close(fd);
            }
            auto info=inspect_input(path,snow_model);if(!info)continue;candidates.push_back(*info);}
        ::closedir(dir);}
    for(const auto& input:candidates){
        out<<"touch_candidate="<<input.name<<"\n"<<"touch_candidate_path="<<input.path<<"\n"<<"touch_candidate_protocol="<<protocol_name(input.protocol)<<"\n"
           <<"touch_candidate_x_min="<<input.x.minimum<<"\ntouch_candidate_x_max="<<input.x.maximum<<"\ntouch_candidate_y_min="<<input.y.minimum<<"\ntouch_candidate_y_max="<<input.y.maximum<<"\n";}
    if(candidates.size()==1){const auto& input=candidates.front();out<<"input=accepted\ninput_protocol="<<protocol_name(input.protocol)
        <<"\ntouch_device="<<input.name<<"\ntouch_x_min="<<input.x.minimum<<"\ntouch_x_max="<<input.x.maximum<<"\ntouch_y_min="<<input.y.minimum<<"\ntouch_y_max="<<input.y.maximum<<"\n";}
    else out<<"input=unsupported\ninput_reason="<<(candidates.empty()?"no supported direct-touch device":"multiple supported touch devices")<<"\n";
    return out.str();
#else
    throw std::runtime_error("设备诊断仅在 Linux Kindle 上可用");
#endif
}
}
