#include "app.hpp"
#include "feed.hpp"
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    if(!rss::CreReader::available()){
        std::cout<<"CRE app checks skipped: engine unavailable\n";
        return 0;
    }
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Store store(":memory:");rss::Canvas canvas(1072,1448,font);
    rss::Feed feed;feed.url="https://cre.example/feed";feed.title="CRE 集成测试";
    const auto feed_id=store.add_feed(feed);
    rss::Entry first;first.guid="cre-first";first.title="CRE 第一篇";first.url="https://cre.example/one";first.published=200;
    first.html="<h1>章节标题</h1><h2>小节</h2><blockquote>引用内容</blockquote><pre>代码片段</pre><p><strong>强调内容</strong></p>";
    for(int i=0;i<120;++i)first.html+="<p>第 "+std::to_string(i)+" 段：新版排版应保存独立页书签，经典阅读字节位置保持不变。</p>";
    first.text=rss::html_to_text(first.html);
    rss::Entry second;second.guid="cre-second";second.title="CRE 第二篇";second.url="https://cre.example/two";second.published=100;
    second.html="<h1>下一篇标题</h1>";for(int i=0;i<80;++i)second.html+="<p>下一篇正文内容，用于确认列表快照中的文章顺序。</p>";
    second.text=rss::html_to_text(second.html);
    rss::ParsedFeed batch;batch.entries={first,second};store.save_entries(feed_id,batch);
    const auto ordered=store.entries(rss::View::All,feed_id);
    assert(ordered.size()==2&&ordered[0].title==first.title&&ordered[1].title==second.title);
    const auto first_id=ordered[0].id,second_id=ordered[1].id;
    {
        rss::App defaults(store,canvas);defaults.show_view(rss::View::All,feed_id);
        defaults.open(first_id);defaults.draw();
        assert(defaults.using_cre());
        assert(store.setting("reader_engine").empty());
    }
    store.set_position(first_id,1000);
    {
        rss::App migrated(store,canvas);migrated.open(first_id);migrated.draw();
        assert(!migrated.using_cre());
        assert(store.entry(first_id)->position==1000);
    }
    store.set_position(first_id,0);
    store.set_setting("reader_engine","classic");
    rss::App app(store,canvas);app.show_view(rss::View::All,feed_id);
    auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
    tap(100,400);assert(app.reading()&&app.current_id()==first_id&&!app.using_cre());
    tap(950,1380);assert(app.reader_page()>0);
    const auto old_offset=store.entry(first_id)->position;assert(old_offset>0);
    tap(536,600);assert(app.reader_tools_open());
    tap(178,1380);assert(app.reader_panel()==rss::App::ReaderPanel::Type);
    tap(930,1162);assert(app.using_cre()&&app.reader_page()==0);
    app.draw();
    if(const auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(dir);canvas.write_pgm((std::filesystem::path(dir)/"cre-controls.pgm").string());}
    tap(900,600);assert(!app.reader_tools_open());
    tap(950,1380);assert(app.reader_page()>0);
    const auto cre_page=app.reader_page();assert(store.entry(first_id)->position==old_offset);
    app.set_reader_status({21,false,true,"09:19"});app.draw();const auto paper=canvas.pixels;
    const auto bookmark=store.setting("reader_cre:"+std::to_string(first_id));
    tap(536,600);tap(400,1380);assert(app.reader_panel()==rss::App::ReaderPanel::Content);
    app.draw();assert(std::equal(paper.begin()+96*1072,paper.begin()+700*1072,canvas.pixels.begin()+96*1072));
    tap(536,600);app.draw();assert(canvas.pixels==paper&&app.reader_page()==cre_page);
    assert(store.setting("reader_cre:"+std::to_string(first_id))==bookmark);
    tap(100,1380);assert(app.reader_page()==cre_page-1);
    tap(536,600);tap(950,50);assert(store.entry(first_id)->starred&&app.reader_page()==cre_page-1);
    tap(536,600);app.next();const auto anchor_before=store.setting("reader_cre:"+std::to_string(first_id));
    tap(536,600);tap(178,1380);tap(960,856);
    assert(store.setting("font_size")=="36"&&app.using_cre()&&app.reader_page()>0);
    assert(store.setting("reader_cre:"+std::to_string(first_id))==anchor_before);
    if(const auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){app.draw();canvas.write_pgm((std::filesystem::path(dir)/"cre-reader.pgm").string());}
    app.back();app.back();app.back();assert(!app.reading());
    tap(100,400);assert(app.current_id()==first_id&&app.using_cre()&&app.reader_page()>0);
    const auto restored=app.reader_page();
    tap(536,600);tap(178,1380);tap(690,1162);assert(!app.using_cre());
    const auto classic_page=app.reader_page();
    assert(store.entry(first_id)->position==old_offset);
    assert(store.setting("reader_cre:"+std::to_string(first_id))!="");
    app.back();app.back();app.back();tap(100,400);assert(app.current_id()==first_id&&!app.using_cre());
    assert(app.reader_page()==classic_page);
    assert(store.entry(first_id)->position<=old_offset); // Classic exit saves its current page start after the font change.
    tap(536,600);tap(178,1380);tap(930,1162);assert(app.using_cre()&&app.reader_page()==restored);
    tap(900,600);
    app.next();app.next();
    for(int i=0;i<1000;++i){const auto before=app.reader_page();app.next();if(app.reader_page()==before)break;}
    assert(app.reader_page()>0);
    app.draw();app.tap(950,1420);
    assert(app.current_id()==second_id&&app.using_cre());
    assert(store.setting("reader_cre:"+std::to_string(first_id)).empty());
    assert(store.entry(first_id)->position==0);

    rss::Entry oversized;oversized.guid="cre-oversized";oversized.title="超限回退";oversized.published=300;
    oversized.text=std::string(512*1024+1,'x');oversized.html=oversized.text;
    rss::ParsedFeed too_large;too_large.entries={oversized};store.save_entries(feed_id,too_large);
    app.back();app.show_view(rss::View::All,feed_id);tap(100,400);
    assert(app.current_id()!=0);
    assert(app.reading()&&!app.using_cre());
    app.back();assert(app.reading());app.back();assert(!app.reading());
    store.set_setting("reader_engine","classic");
    rss::App failed_switch(store,canvas);failed_switch.show_view(rss::View::All,feed_id);
    auto fail_tap=[&](int x,int y){failed_switch.draw();failed_switch.tap(x,y);};
    fail_tap(100,400);failed_switch.next();const auto before_failure=failed_switch.reader_page();
    fail_tap(536,600);fail_tap(178,1380);fail_tap(930,1162);
    assert(!failed_switch.using_cre()&&failed_switch.reader_page()==before_failure);
    assert(store.setting("reader_engine")=="classic");
    std::cout<<"CRE app checks passed\n";
}
