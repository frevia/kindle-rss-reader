#include "app.hpp"
#include "feed.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Canvas canvas(1072,1448,font);
    for(const auto* engine:{"classic","cre"}){
        if(std::string(engine)=="cre"&&!rss::CreReader::available())continue;
        rss::Store store(":memory:");store.set_setting("reader_engine",engine);
        rss::Feed feed;feed.url="https://fulltext.example/feed";feed.title="全文阅读测试";const auto fid=store.add_feed(feed);
        rss::Entry original;original.guid="one";original.url="https://fulltext.example/story";original.title="原文和网页正文分别保存阅读位置";
        for(int i=0;i<90;++i)original.html+="<p>RSS 原文第 "+std::to_string(i)+" 段，保留原有阅读位置，不会被后台完成的正文替换。</p>";
        original.text=rss::html_to_text(original.html);
        rss::ParsedFeed parsed;parsed.entries={original};store.save_entries(fid,parsed);const auto id=store.entries(rss::View::All).front().id;
        rss::App app(store,canvas);app.open(id);app.next();app.next();
        const auto rss_page=app.reader_page();assert(rss_page>0);
        const auto rss_position=store.entry(id)->position;
        const auto rss_anchor=store.setting("reader_cre:"+std::to_string(id));
        auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
        tap(536,600);tap(400,1380);assert(app.reader_panel()==rss::App::ReaderPanel::Content);
        tap(200,1296);assert(app.take_request()=="fulltext:"+std::to_string(id));
        assert(store.fulltext_status(id).state=="pending"&&app.sync_state()==rss::App::SyncState::Running);
        tap(200,1296);assert(app.take_request()=="cancel-sync");
        app.sync_finished(true);assert(!app.using_fulltext()&&app.reader_page()==rss_page);
        rss::Entry full;full.url="https://fulltext.example/final";
        for(int i=0;i<120;++i)full.html+="<p>网页正文第 "+std::to_string(i)+" 段，下载成功后可以离线阅读，切换前后各自保存位置。</p>";
        full.text=rss::html_to_text(full.html);store.save_fulltext(id,full);
        app.sync_started();app.sync_progress(1,1,0,"网页正文已保存");app.sync_finished(false);
        assert(!app.using_fulltext()&&app.reader_page()==rss_page);
        tap(800,1002);assert(app.using_fulltext()&&app.reader_page()==0);
        assert(store.entry(id)->position==rss_position&&store.setting("reader_cre:"+std::to_string(id))==rss_anchor);
        if(const auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){
            std::filesystem::create_directories(dir);app.draw();canvas.write_pgm((std::filesystem::path(dir)/(std::string("fulltext-ready-")+engine+".pgm")).string());
        }
        app.back();app.back();app.next();app.next();const auto web_page=app.reader_page();assert(web_page>0);
        tap(536,600);tap(400,1380);tap(200,1002);assert(!app.using_fulltext()&&app.reader_page()==rss_page);
        tap(800,1002);assert(app.using_fulltext()&&app.reader_page()==web_page);
        app.back();app.back();app.back();assert(!app.reading());
        rss::App reopened(store,canvas);reopened.open(id);assert(reopened.using_fulltext()&&reopened.reader_page()==web_page);
        assert(store.entry(id)->position==rss_position);
        auto fresh=original;fresh.guid="two";parsed.entries={fresh};store.save_entries(fid,parsed);
        const auto other=store.entries(rss::View::All).front().id;assert(other!=id);
        assert(store.queue_fulltext(other));store.save_fulltext(other,full);
        rss::App unread(store,canvas);unread.open(other);assert(unread.using_fulltext());
        rss::App settings(store,canvas);
        auto edit_tap=[&](int x,int y){settings.draw();settings.tap(x,y);};
        edit_tap(150,50);edit_tap(120,154);edit_tap(950,398);edit_tap(250,920);
        assert(store.setting("feed_fulltext:"+std::to_string(fid)).empty());
        edit_tap(150,1380);assert(store.setting("feed_fulltext:"+std::to_string(fid)).empty());
        edit_tap(950,398);edit_tap(250,920);edit_tap(950,1380);
        assert(store.setting("feed_fulltext:"+std::to_string(fid))=="1");
        store.set_starred(id,true);store.remove_feed(fid);assert(store.fulltext_entry(id));
        reopened.open(id);assert(reopened.using_fulltext());
    }
    std::cout<<"fulltext reader checks passed\n";
}
