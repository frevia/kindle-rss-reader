#include "device.hpp"
#include <cassert>
#include <stdexcept>

int main(){
    std::vector<std::uint8_t> before(1072*1448,255),after=before;
    assert(!rss::changed_region(before,after));
    after[1447*1072+1071]=0;
    auto region=rss::changed_region(before,after);
    assert(region&&region->x==1064&&region->y==1440&&region->w==8&&region->h==8);
    after[0]=0;region=rss::changed_region(before,after);
    assert(region&&region->x==0&&region->y==0&&region->w==1072&&region->h==1448);
    bool bad_pixels=false;try{rss::changed_region({},after);}catch(const std::runtime_error&){bad_pixels=true;}assert(bad_pixels);
    rss::FrameLayout good{1072,1448,1088,8,0,0,5792,6301696};rss::validate_kpw3_layout(good);
    for(int variant=0;variant<5;++variant){
        auto bad=good;
        switch(variant){case 0:bad.bpp=16;break;case 1:bad.stride=1071;break;case 2:bad.yoffset=5792;break;
            case 3:bad.memory_size=1;break;case 4:bad.xoffset=17;break;}
        bool rejected=false;try{rss::validate_kpw3_layout(bad);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    rss::TouchDecoder d;
    assert(!d.consume(3,0x39,1));assert(!d.consume(3,0x35,50));assert(!d.consume(3,0x36,100));
    assert(!d.consume(0,0,0));assert(!d.consume(3,0x39,-1));auto point=d.consume(0,0,0);
    assert(point&&point->x==50&&point->y==100);assert(!d.consume(0,0,0));
    d.consume(3,0x2f,1);d.consume(3,0x39,2);d.consume(3,0x35,200);d.consume(3,0x39,-1);assert(!d.consume(0,0,0));
    auto scaled=rss::scale_touch({100,200},0,100,0,200);assert(scaled.x==1071&&scaled.y==1447);
    bool dropped=false;try{d.consume(0,3,0);}catch(const std::runtime_error&){dropped=true;}assert(dropped);
}
