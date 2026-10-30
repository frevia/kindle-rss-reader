#include "device.hpp"
#include <cassert>
#include <algorithm>
#include <stdexcept>

int main(){
    std::vector<std::uint8_t> before(1072*1448,255),after=before;
    assert(!rss::changed_region(before,after));
    after[1447*1072+1071]=0;
    auto region=rss::changed_region(before,after);
    assert(region&&region->x==1064&&region->y==1440&&region->w==8&&region->h==8);
    after[0]=0;region=rss::changed_region(before,after);
    assert(region&&region->x==0&&region->y==0&&region->w==1072&&region->h==1448);
    // Sparse changes, holes within rows, and dense frames retain the exact aligned bounds.
    std::uint32_t seed=12345;
    for(int sample=1;sample<=64;++sample){
        after=before;int left=1072,top=1448,right=0,bottom=0;
        for(int point=0;point<sample;++point){
            seed=seed*1664525u+1013904223u;const int x=seed%1072;
            seed=seed*1664525u+1013904223u;const int y=sample%2?seed%1448:720;
            after[y*1072+x]=0;left=std::min(left,x);right=std::max(right,x);
            top=std::min(top,y);bottom=std::max(bottom,y);
        }
        region=rss::changed_region(before,after);
        assert(region&&region->x==left/8*8&&region->y==top/8*8);
        assert(region->x+region->w==(right/8+1)*8&&region->y+region->h==(bottom/8+1)*8);
    }
    std::fill(after.begin(),after.end(),0);region=rss::changed_region(before,after);
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
    auto ev=[&](int type,int code,int value){return d.consume(type,code,value);};
    auto down=[&](int x,int y){
        assert(!ev(3,0x2f,0));assert(!ev(3,0x39,1));
        assert(!ev(3,0x35,x));assert(!ev(3,0x36,y));assert(!ev(0,0,0));
    };
    auto up=[&](){assert(!ev(3,0x39,-1));return ev(0,0,0);};
    down(10,20);
    assert(!ev(3,0x35,50));assert(!ev(0,0,0));
    assert(!ev(3,0x36,80));assert(!ev(0,0,0));
    assert(!ev(3,0x35,30));assert(!ev(0,0,0));
    assert(!ev(3,0x35,10));assert(!ev(3,0x36,20));assert(!ev(0,0,0));
    auto gesture=up();
    assert(gesture&&gesture->start.x==10&&gesture->start.y==20);
    assert(gesture->end.x==10&&gesture->end.y==20);
    assert(gesture->minimum.x==10&&gesture->minimum.y==20);
    assert(gesture->maximum.x==50&&gesture->maximum.y==80);
    assert(!ev(0,0,0));

    // Some reports carry the final position and tracking-id release in the same frame.
    down(200,300);assert(!ev(3,0x35,350));assert(!ev(3,0x36,310));
    auto release_position=up();
    assert(release_position&&release_position->start.x==200&&release_position->end.x==350);
    assert(release_position->end.y==310&&release_position->maximum.x==350);

    // New tracking IDs reset the path while protocol B axis values remain available.
    down(200,300);auto first_path=up();assert(first_path&&first_path->start.x==200);
    assert(!ev(3,0x2f,0));assert(!ev(3,0x39,2));assert(!ev(0,0,0));
    auto second=up();assert(second&&second->start.x==200&&second->end.x==200);
    assert(second->minimum.x==200&&second->maximum.x==200);

    // Repeated coordinates still form a gesture; an unchanged axis is retained.
    down(40,60);assert(!ev(3,0x35,40));assert(!ev(0,0,0));
    assert(!ev(3,0x36,90));assert(!ev(0,0,0));
    auto axes=up();assert(axes&&axes->start.x==40&&axes->start.y==60);
    assert(axes->end.x==40&&axes->end.y==90);
    assert(axes->maximum.x==40&&axes->maximum.y==90);

    // Do not begin on an incomplete first coordinate report.
    rss::TouchDecoder incomplete;
    assert(!incomplete.consume(3,0x2f,0));assert(!incomplete.consume(3,0x39,3));
    assert(!incomplete.consume(3,0x35,77));assert(!incomplete.consume(0,0,0));
    assert(!incomplete.consume(3,0x36,88));assert(!incomplete.consume(0,0,0));
    assert(!incomplete.consume(3,0x39,-1));auto complete=incomplete.consume(0,0,0);
    assert(complete&&complete->start.x==77&&complete->start.y==88);

    // Any overlapping contact involving another slot cancels the whole primary gesture.
    down(5,6);assert(!ev(3,0x2f,1));assert(!ev(3,0x39,7));
    assert(!ev(3,0x2f,0));assert(!ev(3,0x35,9));assert(!ev(0,0,0));
    assert(!ev(3,0x2f,1));assert(!ev(3,0x39,-1));
    assert(!ev(3,0x2f,0));assert(!up());
    down(11,12);assert(up());

    // A secondary contact already active when slot zero starts also cancels it.
    assert(!ev(3,0x2f,2));assert(!ev(3,0x39,8));
    assert(!ev(3,0x2f,0));assert(!ev(3,0x39,9));assert(!ev(0,0,0));
    assert(!ev(3,0x2f,2));assert(!ev(3,0x39,-1));
    assert(!ev(3,0x2f,0));assert(!up());

    bool dropped=false;try{ev(0,3,0);}catch(const std::runtime_error&){dropped=true;}assert(dropped);
    down(100,200);auto after_drop=up();assert(after_drop&&after_drop->start.x==100);
    auto scaled=rss::scale_touch({100,200},0,100,0,200);assert(scaled.x==1071&&scaled.y==1447);
    rss::TouchGesture raw({0,0});raw.update({50,100});raw.update({25,25});
    auto scaled_gesture=rss::scale_gesture(raw,0,100,0,100);
    assert(scaled_gesture.start.x==0&&scaled_gesture.start.y==0);
    assert(scaled_gesture.end.x==268&&scaled_gesture.end.y==362);
    assert(scaled_gesture.minimum.x==0&&scaled_gesture.minimum.y==0);
    assert(scaled_gesture.maximum.x==536&&scaled_gesture.maximum.y==1447);
    raw.update({101,30});bool bad_scale=false;
    try{rss::scale_gesture(raw,0,100,0,100);}catch(const std::runtime_error&){bad_scale=true;}assert(bad_scale);
}
