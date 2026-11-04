#include "device.hpp"
#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <limits>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>

int main(){
    for(int slot=0;slot<32;++slot){
        rss::TouchDecoder decoder(rss::TouchDecoder::Protocol::TypeB);
        decoder.consume(3,0x2f,slot,0);decoder.consume(3,0x39,100+slot,0);
        decoder.consume(3,0x35,100,0);decoder.consume(3,0x36,200,0);decoder.consume(0,0,0,0);
        auto hold=decoder.poll(600);assert(hold&&hold->phase==rss::TouchPhase::LongPress&&hold->start.x==100);
        decoder.consume(3,0x35,150,610);auto moved=decoder.consume(0,0,0,610);
        assert(moved&&moved->phase==rss::TouchPhase::Move&&moved->end.x==150);
        decoder.consume(3,0x39,-1,620);auto tap=decoder.consume(0,0,0,620);
        assert(tap&&tap->phase==rss::TouchPhase::Complete&&tap->end.x==150&&tap->end.y==200);
    }
    rss::TouchDecoder cached_slots;
    auto contact=[&](int slot,int id,int x,int y){
        cached_slots.consume(3,0x2f,slot);cached_slots.consume(3,0x39,id);
        cached_slots.consume(3,0x35,x);cached_slots.consume(3,0x36,y);cached_slots.consume(0,0,0);
        cached_slots.consume(3,0x39,-1);return cached_slots.consume(0,0,0);
    };
    assert(contact(7,100,100,200));assert(contact(0,101,400,500));
    cached_slots.consume(3,0x2f,7);cached_slots.consume(3,0x39,102);cached_slots.consume(3,0x35,150);cached_slots.consume(0,0,0);
    cached_slots.consume(3,0x39,-1);auto reused_slot=cached_slots.consume(0,0,0);
    assert(reused_slot&&reused_slot->start.x==150&&reused_slot->start.y==200);
    rss::TouchDecoder nonzero_multi;
    nonzero_multi.consume(3,0x2f,7,0);nonzero_multi.consume(3,0x39,1,0);
    nonzero_multi.consume(3,0x35,100,0);nonzero_multi.consume(3,0x36,200,0);nonzero_multi.consume(0,0,0,0);
    assert(nonzero_multi.poll(600));
    nonzero_multi.consume(3,0x2f,0,610);nonzero_multi.consume(3,0x39,2,610);
    nonzero_multi.consume(3,0x35,300,610);nonzero_multi.consume(3,0x36,400,610);
    auto nonzero_cancel=nonzero_multi.consume(0,0,0,610);assert(nonzero_cancel&&nonzero_cancel->phase==rss::TouchPhase::Cancel);
    nonzero_multi.consume(3,0x2f,7,620);nonzero_multi.consume(3,0x39,-1,620);assert(!nonzero_multi.consume(0,0,0,620));
    nonzero_multi.consume(3,0x2f,0,630);nonzero_multi.consume(3,0x35,310,630);assert(!nonzero_multi.consume(0,0,0,630));
    nonzero_multi.consume(3,0x39,-1,640);assert(!nonzero_multi.consume(0,0,0,640));
    nonzero_multi.consume(3,0x2f,31,650);nonzero_multi.consume(3,0x39,3,650);
    nonzero_multi.consume(3,0x35,500,650);nonzero_multi.consume(3,0x36,600,650);nonzero_multi.consume(0,0,0,650);
    nonzero_multi.consume(1,330,0,660);auto fresh=nonzero_multi.consume(0,0,0,660);
    assert(fresh&&fresh->phase==rss::TouchPhase::Complete&&fresh->start.x==500);
    char refresh_program[]="/tmp/rss-fbink-test.XXXXXX";
    const int refresh_fd=mkstemp(refresh_program);assert(refresh_fd>=0);assert(fchmod(refresh_fd,0700)==0);close(refresh_fd);
    auto check_refresh=[&](const std::string& script,bool success){
        {std::ofstream out(refresh_program);out<<"#!/bin/sh\n[ \"$1\" = -q ] && [ \"$2\" = -w ] && [ \"$4\" = GC16 ] || exit 91\n"<<script<<"\n";assert(out.good());}
        bool ok=true;try{rss::refresh_fbink({0,0,8,8},false,refresh_program);}catch(const std::runtime_error&){ok=false;}
        assert(ok==success);
    };
    const std::string optional_wait="printf '%s\\n' '[FBInk] Waiting for update submission is not supported on your device!' >&2\n";
    check_refresh("exit 0",true);check_refresh(optional_wait,true);
    check_refresh("printf '%s\\n' '[FBInk] Failed to wait for completion of update 42!' >&2; exit 0",false);
    check_refresh(optional_wait+"printf '%s\\n' '[FBInk] Failed to wait for completion of update 42!' >&2; exit 0",false);
    check_refresh("printf 'unknown diagnostic' >&2; exit 0",false);
    check_refresh("printf 'unexpected stdout'; exit 0",false);
    check_refresh("exit 7",false);
    check_refresh("awk 'BEGIN {for(i=0;i<4100;i++) printf \"x\"}' >&2",false);
    check_refresh("exit 0",true);assert(unlink(refresh_program)==0);
    for(const auto& sample:std::vector<std::pair<std::string,std::string>>{
        {"B00Fxxxx","Touch"},{"B024xxxx","Paperwhite 1"},{"B0D4xxxx","Paperwhite 2"},
        {"90C6xxxx","Basic 2014"},{"B013xxxx","Voyage"},{"G090G1xxxx","Paperwhite 3"},
        {"G090GCxxxx","Oasis 1"},{"G090LMxxxx","Oasis 2"},{"G090DUxxxx","Basic 2016"},
        {"G090PPxxxx","Paperwhite 4"},{"G0910Lxxxx","Basic 2019"},{"G0911Lxxxx","Oasis 3"},
        {"G091Q0xxxx","Paperwhite 5"},{"G091LGxxxx","Paperwhite 5 SE"},{"G0922Dxxxx","Basic 2022"},
        {"G0927Jxxxx","Scribe 1/2"},{"G093V0xxxx","Scribe 1/2"},{"G093H2xxxx","Colorsoft"},
        {"G09A89xxxx","Basic 2024"},{"G0933Wxxxx","Paperwhite 6"},{"G094PGxxxx","Scribe 3"},
        {"G094VXxxxx","Scribe Colorsoft"}}){
        const auto profile=rss::kindle_profile(sample.first);
        assert(profile.model==sample.second);
        assert(profile.dpi>=167&&profile.dpi<=300);
        assert(profile.snow==(sample.second=="Basic 2019"));
        assert(std::string(profile.historical_firmware)==(sample.second=="Paperwhite 3"?"5.16.2.1.1":"none"));
    }
    assert(std::string(rss::kindle_profile("B013xxxx").display_platform)=="classic");
    assert(std::string(rss::kindle_profile("G090LMxxxx").display_platform)=="zelda");
    assert(std::string(rss::kindle_profile("G090PPxxxx").display_platform)=="rex");
    assert(std::string(rss::kindle_profile("G0927Jxxxx").display_platform)=="mtk");
    for(const auto& serial:{std::string(),std::string("G090G"),std::string("G090G0xxxx"),std::string("B00G1xxx"),
                            std::string("G09D4xxxx"),std::string("G0910L\nxxx"),std::string("G0910L\0xxx",10),std::string(65,'A')}){
        const auto unknown=rss::kindle_profile(serial);assert(std::string(unknown.model)=="Unknown"&&unknown.dpi==0&&!unknown.snow);
    }
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

    // Long press starts at the first complete single-contact coordinate frame.
    rss::TouchDecoder timed;
    auto tev=[&](int type,int code,int value,std::uint64_t now){return timed.consume(type,code,value,now);};
    auto tdown=[&](int x,int y,std::uint64_t now){
        assert(!tev(3,0x2f,0,now));assert(!tev(3,0x39,1,now));
        assert(!tev(3,0x35,x,now));assert(!tev(3,0x36,y,now));assert(!tev(0,0,0,now));
    };
    tdown(10,20,0);
    assert(!timed.poll(599));
    auto long_press=timed.poll(600);
    assert(long_press&&long_press->phase==rss::TouchPhase::LongPress);
    assert(long_press->duration_ms==600&&long_press->hold_ms==600);
    assert(!timed.poll(900));
    assert(!tev(3,0x35,10,950));assert(!tev(0,0,0,950)); // unchanged reports do not emit Move
    assert(!tev(3,0x35,45,960));auto move=tev(0,0,0,960);
    assert(move&&move->phase==rss::TouchPhase::Move&&move->end.x==45);
    assert(move->duration_ms==960&&move->hold_ms==960);
    assert(!tev(3,0x39,-1,1000));auto timed_complete=tev(0,0,0,1000);
    assert(timed_complete&&timed_complete->phase==rss::TouchPhase::Complete);
    assert(timed_complete->duration_ms==1000&&timed_complete->hold_ms==960);

    // A slow swipe that leaves the 24px box before 600ms cannot become a hold.
    rss::TouchDecoder slow;
    auto sev=[&](int type,int code,int value,std::uint64_t now){return slow.consume(type,code,value,now);};
    assert(!sev(3,0x2f,0,1000));assert(!sev(3,0x39,1,1000));
    assert(!sev(3,0x35,10,1000));assert(!sev(3,0x36,10,1000));assert(!sev(0,0,0,1000));
    assert(!sev(3,0x35,40,1100));assert(!sev(0,0,0,1100));
    assert(!slow.poll(1600));
    assert(!sev(3,0x39,-1,1700));auto slow_complete=sev(0,0,0,1700);
    assert(slow_complete&&slow_complete->phase==rss::TouchPhase::Complete);
    assert(slow_complete->hold_ms==100&&slow_complete->duration_ms==700);

    // Exactly 24px of early jitter stays inside the hold box.
    rss::TouchDecoder jitter;
    assert(!jitter.consume(3,0x2f,0,0));assert(!jitter.consume(3,0x39,1,0));
    assert(!jitter.consume(3,0x35,20,0));assert(!jitter.consume(3,0x36,20,0));assert(!jitter.consume(0,0,0,0));
    assert(!jitter.consume(3,0x35,44,200));assert(!jitter.consume(0,0,0,200));
    auto jitter_hold=jitter.poll(600);assert(jitter_hold&&jitter_hold->phase==rss::TouchPhase::LongPress);

    // A second contact cancels an active selection once, and suppresses release output.
    rss::TouchDecoder multi;
    assert(!multi.consume(3,0x2f,0,0));assert(!multi.consume(3,0x39,1,0));
    assert(!multi.consume(3,0x35,10,0));assert(!multi.consume(3,0x36,10,0));assert(!multi.consume(0,0,0,0));
    assert(multi.poll(600));
    assert(!multi.consume(3,0x2f,1,610));assert(!multi.consume(3,0x39,2,610));
    auto cancel=multi.consume(0,0,0,610);
    assert(cancel&&cancel->phase==rss::TouchPhase::Cancel);
    assert(!multi.consume(3,0x39,-1,620));assert(!multi.consume(3,0x2f,0,620));
    assert(!multi.consume(3,0x39,-1,620));assert(!multi.consume(0,0,0,620));
    assert(!multi.consume(3,0x2f,1,630));assert(!multi.consume(3,0x39,-1,630));

    // Release without polling still carries enough hold time for the caller to recognize it.
    rss::TouchDecoder unpolled;
    assert(!unpolled.consume(3,0x2f,0,0));assert(!unpolled.consume(3,0x39,1,0));
    assert(!unpolled.consume(3,0x35,5,0));assert(!unpolled.consume(3,0x36,6,0));assert(!unpolled.consume(0,0,0,0));
    assert(!unpolled.consume(3,0x39,-1,700));auto unpolled_complete=unpolled.consume(0,0,0,700);
    assert(unpolled_complete&&unpolled_complete->hold_ms==700&&unpolled_complete->duration_ms==700);

    // Backward clock values clamp at the last observation and cannot underflow into a hold.
    rss::TouchDecoder backwards;
    assert(!backwards.consume(3,0x2f,0,100));assert(!backwards.consume(3,0x39,1,100));
    assert(!backwards.consume(3,0x35,5,100));assert(!backwards.consume(3,0x36,6,100));assert(!backwards.consume(0,0,0,100));
    assert(!backwards.poll(99));assert(!backwards.poll(699));
    auto backwards_hold=backwards.poll(700);assert(backwards_hold&&backwards_hold->duration_ms==600);

    rss::TouchGesture raw({0,0});raw.update({50,100});raw.update({25,25});
    raw.phase=rss::TouchPhase::Move;raw.duration_ms=1234;raw.hold_ms=567;
    auto scaled_gesture=rss::scale_gesture(raw,0,100,0,100);
    assert(scaled_gesture.start.x==0&&scaled_gesture.start.y==0);
    assert(scaled_gesture.end.x==268&&scaled_gesture.end.y==362);
    assert(scaled_gesture.minimum.x==0&&scaled_gesture.minimum.y==0);
    assert(scaled_gesture.maximum.x==536&&scaled_gesture.maximum.y==1447);
    assert(scaled_gesture.phase==rss::TouchPhase::Move&&scaled_gesture.duration_ms==1234&&scaled_gesture.hold_ms==567);
    raw.update({101,30});bool bad_scale=false;
    try{rss::scale_gesture(raw,0,100,0,100);}catch(const std::runtime_error&){bad_scale=true;}assert(bad_scale);

    // Bounded physical layouts preserve the 1072x1448 logical canvas and exact KPW3 pixels.
    for(auto size: {std::pair<int,int>{600,800},{758,1024},{1072,1448},{1236,1648},{1264,1680},{1404,1872},{1860,2480}}){
        rss::ScreenTransform transform(size.first,size.second);std::vector<std::uint8_t> logical(1072*1448);
        for(std::size_t i=0;i<logical.size();++i)logical[i]=static_cast<std::uint8_t>(i%251);
        auto physical=transform.scale(logical);assert(physical.size()==static_cast<std::size_t>(size.first)*size.second);
        auto white=physical;auto changed=physical;changed.back()=0;auto physical_region=rss::changed_region(white,changed,size.first,size.second);
        assert(physical_region&&physical_region->x+physical_region->w==size.first&&physical_region->y+physical_region->h==size.second);
        if(size.first==1072){assert(physical==logical);}
        auto first=transform.to_logical({transform.x,transform.y});auto last=transform.to_logical({transform.x+transform.width-1,transform.y+transform.height-1});
        assert(first&&first->x==0&&first->y==0);assert(last&&last->x==1071&&last->y==1447);
        assert(!transform.to_logical({transform.x-1,transform.y}));
    }
    for(auto size: {std::pair<int,int>{600,800},{758,1024},{1236,1648},{1264,1680},{1404,1872},{1860,2480}}){
        rss::FrameLayout layout{static_cast<unsigned>(size.first),static_cast<unsigned>(size.second),static_cast<unsigned>(size.first+16),8,0,2,static_cast<unsigned>(size.second+2),static_cast<std::uint64_t>(size.first+16)*(size.second+2)};
        rss::validate_frame_layout(layout);
    }
    for(int invalid=0;invalid<4;++invalid){auto l=rss::FrameLayout{1072,1448,1088,8,0,0,1448,1088ull*1448};
        if(invalid==0)l.stride=8193;if(invalid==1)l.xoffset=17;if(invalid==2)l.yoffset=std::numeric_limits<std::uint32_t>::max();if(invalid==3)l.memory_size=64*1024*1024+1;
        bool rejected=false;try{rss::validate_frame_layout(l);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    auto argv=rss::fbink_refresh_args({8,16,24,32},true);
    assert((argv==std::vector<std::string>{"/mnt/us/libkh/bin/fbink","-q","-w","-W","GC16","-f","--refresh","top=16,left=8,width=24,height=32"}));
    auto partial=rss::fbink_refresh_args({0,0,8,8},false);assert(partial.size()==7&&partial[5]=="--refresh");

    // Type A frame reports, including the empty SYN_REPORT lift, share click and hold semantics.
    rss::TouchDecoder type_a(rss::TouchDecoder::Protocol::TypeA);
    auto ae=[&](int type,int code,int value,std::uint64_t now=0){return type_a.consume(type,code,value,now);};
    assert(!ae(3,0x35,100));assert(!ae(3,0x36,200));assert(!ae(0,2,0));assert(!ae(0,0,0,0));
    assert(!ae(3,0x35,110,100));assert(!ae(3,0x36,210,100));assert(!ae(0,2,0,100));assert(!ae(0,0,0,100));
    auto a_up=ae(0,0,0,120);assert(a_up&&a_up->phase==rss::TouchPhase::Complete);
    assert(!ae(3,0x35,100));assert(!ae(3,0x36,200));assert(!ae(0,2,0));assert(!ae(0,0,0,300));
    auto a_hold=type_a.poll(900);assert(a_hold&&a_hold->phase==rss::TouchPhase::LongPress);
    assert(!ae(3,0x35,101,905));assert(!ae(3,0x36,201,905));assert(!ae(0,2,0,905));auto a_move=ae(0,0,0,905);
    assert(a_move&&a_move->phase==rss::TouchPhase::Move);
    assert(!ae(3,0x35,100,910));assert(!ae(3,0x36,200,910));assert(!ae(0,2,0,910));
    assert(!ae(3,0x35,300,910));assert(!ae(3,0x36,400,910));assert(!ae(0,2,0,910));
    auto a_cancel=ae(0,0,0,910);assert(a_cancel&&a_cancel->phase==rss::TouchPhase::Cancel);
    assert(!ae(3,0x35,301,920));assert(!ae(3,0x36,401,920));assert(!ae(0,2,0,920));assert(!ae(0,0,0,920));
    assert(!ae(0,0,0,930)); // the remaining finger cannot take over the cancelled gesture
    bool a_drop=false;try{ae(0,3,0);}catch(const std::runtime_error&){a_drop=true;}assert(a_drop);
    rss::TouchDecoder a_button(rss::TouchDecoder::Protocol::TypeA);
    assert(!a_button.consume(3,0x35,10));assert(!a_button.consume(3,0x36,20));assert(!a_button.consume(0,2,0));assert(!a_button.consume(0,0,0));
    assert(!a_button.consume(1,330,0));auto a_button_up=a_button.consume(0,0,0);
    assert(a_button_up&&a_button_up->phase==rss::TouchPhase::Complete);

    rss::TouchDecoder single(rss::TouchDecoder::Protocol::Single);
    auto se=[&](int type,int code,int value,std::uint64_t now=0){return single.consume(type,code,value,now);};
    assert(!se(1,330,1));assert(!se(3,0,20));assert(!se(3,1,30));assert(!se(0,0,0,10));
    assert(!single.poll(609));auto s_hold=single.poll(610);assert(s_hold&&s_hold->phase==rss::TouchPhase::LongPress);
    assert(!se(3,0,25,615));auto s_move=se(0,0,0,615);assert(s_move&&s_move->phase==rss::TouchPhase::Move&&s_move->end.x==25);
    assert(!se(1,330,0,620));auto s_up=se(0,0,0,620);assert(s_up&&s_up->phase==rss::TouchPhase::Complete);

    rss::TouchDecoder btn_release;
    assert(!btn_release.consume(3,0x2f,0,0));assert(!btn_release.consume(3,0x39,4,0));
    assert(!btn_release.consume(3,0x35,40,0));assert(!btn_release.consume(3,0x36,50,0));assert(!btn_release.consume(0,0,0,0));
    assert(!btn_release.consume(1,330,0,20));auto button_up=btn_release.consume(0,0,0,20);
    assert(button_up&&button_up->phase==rss::TouchPhase::Complete);

    // Global contact lift also applies when the driver's last selected slot is not zero.
    rss::TouchDecoder cross_slot;
    cross_slot.consume(3,0x39,1);cross_slot.consume(3,0x35,10);cross_slot.consume(3,0x36,20);cross_slot.consume(0,0,0);
    cross_slot.consume(3,0x2f,1);cross_slot.consume(1,330,0);
    auto cross_up=cross_slot.consume(0,0,0);assert(cross_up&&cross_up->phase==rss::TouchPhase::Complete);
    assert(!cross_slot.poll(1000));

    // A Type A driver may omit the final SYN_MT_REPORT; empty packets are not extra fingers.
    rss::TouchDecoder tail(rss::TouchDecoder::Protocol::TypeA);
    tail.consume(3,0x35,40);tail.consume(3,0x36,50);assert(!tail.consume(0,0,0));
    assert(tail.poll(600));
    tail.consume(3,0x35,41,610);tail.consume(3,0x36,51,610);tail.consume(0,2,0,610);tail.consume(0,2,0,610);
    auto tail_move=tail.consume(0,0,0,610);assert(tail_move&&tail_move->phase==rss::TouchPhase::Move);
    tail.consume(3,0x35,41,620);tail.consume(3,0x36,51,620);tail.consume(0,2,0,620);
    tail.consume(3,0x35,400,620);tail.consume(3,0x36,500,620);
    auto tail_cancel=tail.consume(0,0,0,620);assert(tail_cancel&&tail_cancel->phase==rss::TouchPhase::Cancel);
    tail.consume(3,0x35,41,630);tail.consume(3,0x36,51,630);assert(!tail.consume(0,0,0,630));
    assert(!tail.poll(2000));assert(!tail.consume(0,0,0,2010));

    // Multi-finger tool reports cancel even a single-touch driver's active long press.
    rss::TouchDecoder tools(rss::TouchDecoder::Protocol::Single);
    tools.consume(1,330,1);tools.consume(3,0,20);tools.consume(3,1,30);tools.consume(0,0,0);
    assert(tools.poll(600));tools.consume(1,333,1,610);
    auto tool_cancel=tools.consume(0,0,0,610);assert(tool_cancel&&tool_cancel->phase==rss::TouchPhase::Cancel);
    tools.consume(1,330,0,620);assert(!tools.consume(0,0,0,620));

    // A hold that leaves the viewport must withdraw its selection, including on re-entry.
    rss::ScreenTransform small(600,800);
    std::vector<std::uint8_t> thin(1072*1448,255);
    for(int row=0;row<1448;++row)thin[row*1072+1]=0;
    auto thin_pixels=small.scale(thin);bool vertical_visible=false;
    for(int x=small.x;x<small.x+small.width;++x)if(thin_pixels[(small.y+small.height/2)*600+x]<255)vertical_visible=true;
    assert(vertical_visible);
    std::fill(thin.begin(),thin.end(),255);std::fill(thin.begin()+1072,thin.begin()+2144,0);
    thin_pixels=small.scale(thin);bool horizontal_visible=false;
    for(int y=small.y;y<small.y+small.height;++y)if(thin_pixels[y*600+small.x+small.width/2]<255)horizontal_visible=true;
    assert(horizontal_visible);
    rss::TouchGesture border({small.x+10,small.y+10});border.phase=rss::TouchPhase::LongPress;
    assert(rss::map_touch_gesture(border,small).phase==rss::TouchPhase::LongPress);
    border.update({small.x-1,small.y+10});border.phase=rss::TouchPhase::Move;
    assert(rss::map_touch_gesture(border,small).phase==rss::TouchPhase::Cancel);
    border.update({small.x+20,small.y+10});border.phase=rss::TouchPhase::Complete;
    assert(rss::map_touch_gesture(border,small).phase==rss::TouchPhase::Cancel);

    // Normalize controller ranges before decoding: the same small movement retains a hold.
    for(int raw_max:{1023,4095,32767}){
        rss::TouchDecoder calibrated(rss::TouchDecoder::Protocol::TypeB,{13,13});
        const auto position=[&](int raw){return rss::scale_touch({raw,raw},0,raw_max,0,raw_max,600,800);};
        auto start=position(raw_max/2),jitter=position(raw_max/2+raw_max/100);
        calibrated.consume(3,0x39,1);calibrated.consume(3,0x35,start.x);calibrated.consume(3,0x36,start.y);calibrated.consume(0,0,0);
        calibrated.consume(3,0x35,jitter.x,200);calibrated.consume(3,0x36,jitter.y,200);calibrated.consume(0,0,0,200);
        assert(calibrated.poll(600));
    }

    // Basic 2019 model recognition is bounded, ASCII-only, and does not infer unknown revisions.
    for(const auto* code:{"10L","0WF","0WG","0WH","0WJ","0VB"})assert(rss::kindle_snow_serial(std::string("G09")+code+"0000000000"));
    assert(!rss::kindle_snow_serial("G09XYZ0000000000"));
    assert(!rss::kindle_snow_serial("B09" "10L" "0000000000"));
    assert(!rss::kindle_snow_serial("9G9" "10L" "0000000000"));
    assert(!rss::kindle_snow_serial("G09" "10L" "0000000000000000000000000000000000000000000000000000000000000"));
    assert(!rss::kindle_snow_serial("G09" "10L" "0000000000\n"));
    assert(rss::select_touch_protocol(true,true,true,true)==rss::TouchDecoder::Protocol::TypeB);
    assert(rss::select_touch_protocol(false,true,true,true)==rss::TouchDecoder::Protocol::Snow);
    assert(rss::select_touch_protocol(false,true,false,true)==rss::TouchDecoder::Protocol::TypeA);
    assert(rss::select_touch_protocol(false,true,true,false)==rss::TouchDecoder::Protocol::TypeA);

    rss::TouchDecoder snow(rss::TouchDecoder::Protocol::Snow);
    auto snow_ev=[&](int type,int code,int value,std::uint64_t now=0){return snow.consume(type,code,value,now);};
    assert(!snow_ev(3,0x39,7,0));assert(!snow_ev(3,0x35,10,0));assert(!snow_ev(3,0x36,20,0));assert(!snow_ev(0,0,0,0));
    assert(!snow_ev(3,0x39,7,100));assert(!snow_ev(3,0x35,12,100));assert(!snow_ev(0,0,0,100)); // same ID and one axis keep the original start
    assert(!snow_ev(3,0x36,24,200));assert(!snow_ev(0,0,0,200)); // omitted ID continues the current contact
    assert(!snow.poll(599));auto snow_hold=snow.poll(600);assert(snow_hold&&snow_hold->phase==rss::TouchPhase::LongPress);
    assert(!snow_ev(3,0x39,7,610));assert(!snow_ev(3,0x35,40,610));assert(!snow_ev(0,2,0,610));
    auto snow_move=snow_ev(0,0,0,610);assert(snow_move&&snow_move->phase==rss::TouchPhase::Move&&snow_move->end.x==40&&snow_move->end.y==24);
    assert(!snow_ev(3,0x36,25,620));snow_move=snow_ev(0,0,0,620);
    assert(snow_move&&snow_move->phase==rss::TouchPhase::Move&&snow_move->end.x==40&&snow_move->end.y==25);
    assert(!snow_ev(3,0x39,-1,700));auto snow_up=snow_ev(0,0,0,700);
    assert(snow_up&&snow_up->phase==rss::TouchPhase::Complete&&snow.protocol()==rss::TouchDecoder::Protocol::TypeB);

    rss::TouchDecoder snow_primary(rss::TouchDecoder::Protocol::Snow);
    snow_primary.consume(3,0x39,19,0);snow_primary.consume(3,0x35,30,0);snow_primary.consume(3,0x36,40,0);snow_primary.consume(0,0,0,0);
    auto snow_first_up=snow_primary.consume(3,0x39,-1,30);auto snow_first=snow_primary.consume(0,0,0,30);
    assert(snow_first&&snow_first->phase==rss::TouchPhase::Complete&&snow_first->start.x==30&&snow_first->start.y==40);
    snow_primary.consume(3,0x39,12345,100);snow_primary.consume(3,0x35,31,100);snow_primary.consume(0,0,0,100);
    snow_primary.consume(3,0x39,-1,120);auto standard_after_snow=snow_primary.consume(0,0,0,120);
    assert(standard_after_snow&&standard_after_snow->start.x==31&&standard_after_snow->end.y==40);

    // A second ID cancels a long press once; the surviving finger cannot complete a click.
    rss::TouchDecoder snow_multi(rss::TouchDecoder::Protocol::Snow);
    snow_multi.consume(3,0x39,8,0);snow_multi.consume(3,0x35,5,0);snow_multi.consume(3,0x36,6,0);snow_multi.consume(0,0,0,0);
    assert(snow_multi.poll(600));snow_multi.consume(3,0x39,24,610);
    auto snow_cancel=snow_multi.consume(0,0,0,610);assert(snow_cancel&&snow_cancel->phase==rss::TouchPhase::Cancel);
    assert(!snow_multi.consume(3,0x39,-1,620));assert(!snow_multi.consume(0,0,0,620));
    snow_multi.consume(3,0x2f,0,630);snow_multi.consume(3,0x39,-1,630);
    assert(!snow_multi.consume(0,0,0,630));

    rss::TouchDecoder snow_button(rss::TouchDecoder::Protocol::Snow);
    snow_button.consume(3,0x39,3);snow_button.consume(3,0x35,10);snow_button.consume(3,0x36,20);snow_button.consume(0,0,0);
    snow_button.consume(1,330,0);auto snow_global_up=snow_button.consume(0,0,0);
    assert(snow_global_up&&snow_global_up->phase==rss::TouchPhase::Complete);
    snow_button.consume(3,0x39,31);snow_button.consume(3,0x35,40);snow_button.consume(3,0x36,50);snow_button.consume(0,0,0);
    auto snow_button_up=snow_button.consume(3,0x39,-1);auto snow_button_done=snow_button.consume(0,0,0);
    assert(snow_button_done&&snow_button_done->phase==rss::TouchPhase::Complete&&snow_button_done->start.x==40);

    rss::TouchDecoder snow_drop(rss::TouchDecoder::Protocol::Snow);bool snow_dropped=false;
    try{snow_drop.consume(0,3,0);}catch(const std::runtime_error&){snow_dropped=true;}assert(snow_dropped);
    snow_drop.consume(3,0x39,5);snow_drop.consume(3,0x35,50);snow_drop.consume(3,0x36,60);snow_drop.consume(0,0,0);
    auto snow_after_drop=snow_drop.consume(3,0x39,-1);auto snow_after_drop_done=snow_drop.consume(0,0,0);
    assert(snow_after_drop_done&&snow_after_drop_done->start.x==50);

    rss::TouchDecoder snow_invalid(rss::TouchDecoder::Protocol::Snow);
    for(int id:{-2,32}){bool rejected=false;try{snow_invalid.consume(3,0x39,id);}catch(const std::runtime_error&){rejected=true;}assert(rejected);}
    snow_invalid.consume(3,0x39,0);snow_invalid.consume(3,0x35,1);snow_invalid.consume(3,0x36,2);snow_invalid.consume(0,0,0);
    snow_invalid.consume(3,0x39,-1);snow_invalid.consume(0,0,0);assert(snow_invalid.protocol()==rss::TouchDecoder::Protocol::TypeB);

    rss::TouchDecoder snow_slot(rss::TouchDecoder::Protocol::Snow);
    snow_slot.consume(3,0x2f,0);assert(snow_slot.protocol()==rss::TouchDecoder::Protocol::TypeB);
    rss::TouchDecoder snow_active_slot(rss::TouchDecoder::Protocol::Snow);
    snow_active_slot.consume(3,0x39,4);snow_active_slot.consume(3,0x35,1);snow_active_slot.consume(3,0x36,2);snow_active_slot.consume(0,0,0);
    bool unsafe_switch=false;try{snow_active_slot.consume(3,0x2f,0);}catch(const std::runtime_error&){unsafe_switch=true;}assert(unsafe_switch);

    // All valid Snow IDs fit the shared slot bounds, including zero as a secondary contact.
    rss::TouchDecoder all_ids(rss::TouchDecoder::Protocol::Snow);
    all_ids.consume(3,0x39,31);all_ids.consume(3,0x35,10);all_ids.consume(3,0x36,20);all_ids.consume(0,0,0);
    for(int id=0;id<31;++id)all_ids.consume(3,0x39,id,10);
    assert(!all_ids.consume(0,0,0,10));assert(!all_ids.poll(1000));
    all_ids.consume(1,330,0,1010);assert(!all_ids.consume(0,0,0,1010));
    all_ids.consume(3,0x39,0,1100);all_ids.consume(3,0x35,30,1100);all_ids.consume(3,0x36,40,1100);all_ids.consume(0,0,0,1100);
    all_ids.consume(1,330,0,1120);auto after_all_ids=all_ids.consume(0,0,0,1120);
    assert(after_all_ids&&after_all_ids->start.x==30&&after_all_ids->end.y==40);

    rss::TouchDecoder unfinished(rss::TouchDecoder::Protocol::Snow);
    unfinished.consume(3,0x39,4);unfinished.consume(3,0x35,10);unfinished.consume(3,0x36,20);unfinished.consume(0,0,0);
    unfinished.consume(1,330,0);
    bool bad_frame=false;try{unfinished.consume(3,0x39,5);}catch(const std::runtime_error&){bad_frame=true;}assert(bad_frame);
}
