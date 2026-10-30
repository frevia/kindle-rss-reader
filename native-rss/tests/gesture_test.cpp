#include "app.hpp"
#include "feed.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
// Exercise the same decoder -> screen scaling -> App path used by the Kindle event loop.
void drag(rss::App& app,std::initializer_list<rss::TouchPoint> points){
    assert(points.size()>0);
    rss::TouchDecoder decoder;
    assert(!decoder.consume(3,47,0));assert(!decoder.consume(3,57,1));
    for(auto point:points){
        assert(!decoder.consume(3,53,point.x));assert(!decoder.consume(3,54,point.y));
        assert(!decoder.consume(0,0,0));
    }
    assert(!decoder.consume(3,57,-1));
    auto gesture=decoder.consume(0,0,0);assert(gesture);
    app.touch(rss::scale_gesture(*gesture,0,1071,0,1447));
    assert(!decoder.consume(0,0,0));
}
void check_engine(const std::string& font,bool use_cre){
    rss::Store store(":memory:");rss::Canvas canvas(1072,1448,font);
    store.set_setting("reader_engine",use_cre?"cre":"classic");
    rss::Feed feed;feed.url="https://gesture.invalid/feed";feed.title="滑动测试";
    const auto feed_id=store.add_feed(feed);
    rss::Entry entry;entry.guid="long";entry.title="正文滑动翻页";entry.published=200;
    for(int i=0;i<100;++i)entry.html+="<p>第 "+std::to_string(i)+" 段。左右滑动只翻一页，阅读位置随翻页保存。工具栏和按钮仍然可以轻点操作。</p>";
    entry.text=rss::html_to_text(entry.html);
    rss::Entry second;second.guid="next";second.title="下一篇";second.text="下一篇不能被滑动自动打开。";second.published=100;
    rss::ParsedFeed batch;batch.entries={entry,second};store.save_entries(feed_id,batch);
    const auto id=store.entries(rss::View::All)[0].id;
    rss::App app(store,canvas);app.show_view(rss::View::All);app.draw();
    drag(app,{{800,300},{400,300}});assert(!app.reading()&&!app.dirty); // List drag is not a row tap.
    app.open(id);app.draw();assert(app.using_cre()==use_cre&&app.reader_page()==0);
    drag(app,{{400,500},{800,500}});assert(app.reader_page()==0&&!app.dirty); // First page clamps.
    drag(app,{{700,600},{600,600},{500,610}});assert(app.reader_page()==1&&!app.reader_tools_open());
    const auto bookmark=use_cre?store.setting("reader_cre:"+std::to_string(id)):std::to_string(store.entry(id)->position);
    assert(!bookmark.empty()&&bookmark!="0");
    app.draw();drag(app,{{500,600},{600,600},{700,610}});assert(app.reader_page()==0);
    app.draw();drag(app,{{700,600},{620,600}});assert(app.reader_page()==1); // Minimum swipe distance.
    app.draw();
    const auto unchanged=[&](std::initializer_list<rss::TouchPoint> path){
        const auto page=app.reader_page();const auto article=app.current_id();
        drag(app,path);
        assert(app.reader_page()==page&&app.current_id()==article&&!app.reader_tools_open()&&!app.dirty);
    };
    unchanged({{700,600},{630,600}}); // Too long for a tap, too short for a swipe.
    unchanged({{500,350},{505,700}}); // Vertical movement must not open tools.
    unchanged({{700,600},{600,700}}); // Diagonal movement.
    unchanged({{500,600},{850,600},{505,605}}); // Return to start retains travelled bounds.
    unchanged({{800,50},{950,50}}); // Do not activate favorite/back at release.
    unchanged({{200,50},{62,54}});
    unchanged({{700,1400},{950,1400}});
    unchanged({{700,600},{700,20},{500,600}}); // A path crossing the header is not a reading swipe.
    app.show_error("测试错误");app.draw();drag(app,{{700,600},{500,600}});assert(app.reader_page()==1&&!app.dirty);
    app.back();app.draw();
    drag(app,{{536,600},{543,610}});assert(app.reader_tools_open()); // Finger jitter remains a tap.
    app.draw();drag(app,{{700,600},{500,600}});assert(app.reader_tools_open()&&app.reader_page()==1&&!app.dirty);
    drag(app,{{178,1400}});app.draw();assert(app.reader_panel()==rss::App::ReaderPanel::Type);
    drag(app,{{700,600},{500,600}});assert(app.reader_page()==1&&!app.dirty);
    app.back();app.back();app.draw();assert(!app.reader_tools_open());
    drag(app,{{950,600}});assert(app.reader_page()==2); // Existing edge tap still works.
    app.draw();drag(app,{{100,600}});assert(app.reader_page()==1);
    app.back();app.draw();app.open(id);app.draw();assert(app.reader_page()==1); // Saved position survives reopening.
    if(const auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){
        std::filesystem::create_directories(dir);
        canvas.write_pgm((std::filesystem::path(dir)/(use_cre?"swipe-cre.pgm":"swipe-classic.pgm")).string());
    }
    for(int i=0;i<1000;++i){const auto page=app.reader_page();app.next();if(page==app.reader_page())break;}
    assert(app.reader_page()>1);app.draw();
    const auto last=app.reader_page();
    const auto saved=use_cre?store.setting("reader_cre:"+std::to_string(id)):std::to_string(store.entry(id)->position);
    drag(app,{{700,600},{500,600}});
    assert(app.reader_page()==last&&app.current_id()==id&&!app.dirty);
    assert(saved==(use_cre?store.setting("reader_cre:"+std::to_string(id)):std::to_string(store.entry(id)->position)));
}
}
int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    check_engine(font,false);
    if(rss::CreReader::available())check_engine(font,true);
    std::cout<<"Gesture decoding, reader paging and tap isolation checks passed\n";
}
