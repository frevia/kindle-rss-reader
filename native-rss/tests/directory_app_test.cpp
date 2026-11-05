#include "app.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc,char** argv){
    assert(argc==2);const std::string server=argv[1];
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Store store(":memory:");store.set_setting("reader_engine","classic");store.add_group("我的收藏");
    rss::Directory directory;directory.title="本机博客合集";directory.url=server+"/feeds.opml";
    const auto id=rss::save_directory(store,directory);const std::atomic<bool> cancel{false};rss::refresh_directory(store,id,cancel);
    for(const auto* name:{"extra-a","extra-b"}){rss::Directory extra;extra.title=name;extra.url=server+"/"+name+".opml";rss::save_directory(store,extra);}
    rss::Canvas canvas(1072,1448,font);rss::App app(store,canvas);
    const auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
    const auto shot=[&](const char* name){app.draw();if(const auto* dir=std::getenv("RSS_TEST_SNAPSHOTS")){
        std::filesystem::create_directories(dir);canvas.write_pgm((std::filesystem::path(dir)/name).string());
    }};
    const auto replace=[&](const std::string& text){for(int i=0;i<2048;++i)app.backspace();app.text_input(text);app.submit();};
    tap(100,1392);tap(140,966);assert(app.screen()==rss::App::Screen::SubscriptionHome);
    shot("directory-subscriptions.pgm");tap(200,536);assert(app.screen()==rss::App::Screen::Discovery);shot("directory-search.pgm");
    assert(store.feeds().empty());
    const auto catalogs_before=rss::directory_catalogs(store);
    assert(catalogs_before.size()==5);
    for(const auto& item:catalogs_before)assert(item.kind==rss::DirectoryKind::Opml&&item.id!="feedspot"&&item.id!="rsshub");
    assert(catalogs_before[0].url=="https://raw.githubusercontent.com/timqian/chinese-independent-blogs/master/feed.opml");
    assert(catalogs_before[1].url=="https://raw.githubusercontent.com/JackyST0/awesome-rsshub-routes/main/feeds.opml");
    tap(200,352);assert(app.screen()==rss::App::Screen::DirectoryScope);shot("directory-scope.pgm");app.back();assert(app.screen()==rss::App::Screen::Discovery);
    tap(200,352);tap(200,784);assert(app.screen()==rss::App::Screen::Discovery); // Select the custom OPML library.
    tap(200,352);tap(200,352);assert(app.screen()==rss::App::Screen::Discovery); // Restore all OPML libraries.
    tap(200,1380);assert(app.screen()==rss::App::Screen::Discovery&&store.feeds().empty()); // Removed manual-add footer action.
    app.draw();const auto first=canvas.pixels;app.next();app.draw();assert(canvas.pixels!=first);app.previous();app.draw();assert(canvas.pixels==first);
    tap(200,260);app.text_input("博客 7");app.submit();shot("directory-found.pgm");
    tap(950,600);assert(app.screen()==rss::App::Screen::Discovery); // The native feed form overlays its origin.
    shot("directory-subscribe-form.pgm");tap(200,770);assert(app.screen()==rss::App::Screen::GroupPicker);
    tap(200,430);assert(app.screen()==rss::App::Screen::Discovery);tap(950,136);
    auto subscribed=store.feeds();assert(subscribed.size()==1&&subscribed[0].title=="博客 7"&&subscribed[0].group=="我的收藏");
    assert(app.screen()==rss::App::Screen::Discovery);shot("directory-subscribed.pgm");
    tap(950,600);assert(store.feeds().size()==1); // Already-subscribed action is disabled.
    tap(950,260); // clear search
    tap(200,440);assert(app.screen()==rss::App::Screen::DirectoryTopics);shot("directory-topics.pgm");
    app.back();assert(app.screen()==rss::App::Screen::Discovery);
    tap(200,260);app.text_input("不存在");app.submit();shot("directory-empty.pgm");
    app.back();assert(app.screen()==rss::App::Screen::SubscriptionHome);tap(200,724);assert(app.screen()==rss::App::Screen::Directories);shot("directory-management.pgm");
    app.draw();const auto idle_listing=canvas.pixels;
    app.directory_started();shot("directory-downloading.pgm");
    for(int y=1120;y<1216;++y)for(int x=32;x<780;++x)assert(canvas.pixels[y*1072+x]==idle_listing[y*1072+x]);
    app.directory_finished(true);
    // OPML entries open a detail page with the real built-in URL and custom OPML can be edited.
    tap(950,334);assert(app.screen()==rss::App::Screen::DirectoryDetail);shot("directory-detail.pgm");
    app.back();assert(app.screen()==rss::App::Screen::Directories);
    tap(950,136);assert(app.screen()==rss::App::Screen::DirectoryEdit);shot("directory-add.pgm");
    tap(200,380);replace("我的 OPML 源库");tap(200,600);replace("https://example.com/collection");
    const auto unchanged=store.setting("directory_catalogs_v1");
    bool invalid=false;try{tap(950,136);}catch(const std::runtime_error& error){invalid=std::string(error.what()).find(".opml")!=std::string::npos;}
    assert(invalid&&app.screen()==rss::App::Screen::DirectoryEdit&&store.setting("directory_catalogs_v1")==unchanged);
    tap(200,600);replace("https://example.com/collection.opml");
    tap(950,136);assert(app.screen()==rss::App::Screen::Directories);
    const auto catalogs=rss::directory_catalogs(store);assert(catalogs.size()==6);
    auto added=catalogs.back();assert(added.title=="我的 OPML 源库"&&added.kind==rss::DirectoryKind::Opml&&added.url=="https://example.com/collection.opml");
    // New custom entries remain reachable on the page chosen by the save action.
    shot("directory-custom.pgm");tap(950,334);assert(app.screen()==rss::App::Screen::DirectoryDetail);
    app.back();assert(app.screen()==rss::App::Screen::Directories);
    // Page two contains the new OPML library; the old footer cannot save a draft.
    tap(962,1392);tap(950,434);assert(app.screen()==rss::App::Screen::DirectoryEdit);
    tap(200,380);replace("未保存的草稿");tap(950,1380);assert(app.screen()==rss::App::Screen::DirectoryEdit);
    app.back();assert(rss::directory_catalogs(store).back().title=="我的 OPML 源库");
    tap(962,1392);tap(950,434);tap(200,1200);shot("directory-remove.pgm");app.back();assert(rss::directory_catalogs(store).size()==6);
    tap(200,1200);tap(900,1392);assert(rss::directory_catalogs(store).size()==5&&store.feeds().size()==1);
    app.back();assert(app.screen()==rss::App::Screen::SubscriptionHome);tap(200,536);assert(app.screen()==rss::App::Screen::Discovery);
    app.directory_started();app.draw();tap(950,136);assert(app.take_request()=="cancel-directory");app.back();app.back();
    app.set_reader_status({100,false,true,"12:00"});app.maybe_auto_sync(2000000000);assert(app.take_request().empty());
    app.directory_finished(true);assert(!app.directory_running());
    // Background completion never replaces an open editor or commits its draft.
    tap(100,1392);tap(140,966);tap(200,536);app.back();tap(200,724);tap(950,786);
    assert(app.screen()==rss::App::Screen::DirectoryEdit);app.directory_started();app.directory_finished(false,"下载失败");
    assert(app.screen()==rss::App::Screen::DirectoryEdit&&rss::directory_catalogs(store).size()==5);app.back();
    // Legacy custom records remain readable; new additions cannot create website entries.
    {
        rss::Store old_sites(":memory:");old_sites.set_setting("reader_engine","classic");
        const std::string metadata="<directories><directory id=\"legacy-site\" title=\"旧网站入口\" kind=\"website\" url=\"https://legacy.example/collection\"/></directories>";
        old_sites.set_setting("directory_catalogs_v1",metadata);
        rss::App legacy_browser(old_sites,canvas);
        const auto click=[&](int x,int y){legacy_browser.draw();legacy_browser.tap(x,y);};
        click(100,1392);click(140,966);click(200,724);click(950,754);
        assert(legacy_browser.screen()==rss::App::Screen::DirectoryWebsite);
        legacy_browser.directory_started();bool blocked=false;try{click(500,1368);}catch(const std::runtime_error&){blocked=true;}
        assert(blocked&&!legacy_browser.quit);legacy_browser.directory_finished(true);click(500,1368);
        assert(legacy_browser.quit&&legacy_browser.open_website=="https://legacy.example/collection");
        assert(old_sites.setting("directory_catalogs_v1")==metadata);
        rss::App convert(old_sites,canvas);
        const auto edit=[&](int x,int y){convert.draw();convert.tap(x,y);};
        edit(100,1392);edit(140,966);edit(200,724);edit(950,834);
        assert(convert.screen()==rss::App::Screen::DirectoryEdit);
        bool invalid=false;try{edit(950,136);}catch(const std::runtime_error&){invalid=true;}
        assert(invalid&&old_sites.setting("directory_catalogs_v1")==metadata);
        edit(200,600);for(int i=0;i<2048;++i)convert.backspace();convert.text_input("https://legacy.example/feeds.opml");convert.submit();edit(950,136);
        const auto converted=rss::directory_catalogs(old_sites).back();
        assert(converted.id=="legacy-site"&&converted.kind==rss::DirectoryKind::Opml&&converted.url=="https://legacy.example/feeds.opml");
    }

    {
        rss::Store defaults(":memory:");defaults.set_setting("reader_engine","classic");
        rss::App fresh(defaults,canvas);
        const auto click=[&](int x,int y){fresh.draw();fresh.tap(x,y);};
        click(100,1392);click(140,966);click(200,724);fresh.draw();
        assert(fresh.screen()==rss::App::Screen::Directories&&rss::directory_catalogs(defaults).size()==2);
        if(const auto* dir=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(dir)/"directory-defaults.pgm").string());
    }

    {
        rss::Store long_store(":memory:");long_store.set_setting("reader_engine","classic");
        rss::Directory long_directory;long_directory.title="很长的源库地址";
        long_directory.url="https://example.test/"+std::string(1800,'a')+"/feeds.opml";
        rss::save_directory(long_store,long_directory);
        rss::App long_app(long_store,canvas);
        const auto click=[&](int x,int y){long_app.draw();long_app.tap(x,y);};
        click(100,1392);click(140,966);click(200,724);click(950,704);
        assert(long_app.screen()==rss::App::Screen::DirectoryDetail);long_app.draw();
        if(const auto* dir=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(dir)/"directory-long-detail.pgm").string());
        click(500,1368);assert(long_app.screen()==rss::App::Screen::Discovery&&long_app.take_request().empty());
    }

    {
        rss::Store legacy(":memory:");legacy.set_setting("reader_engine","classic");
        const auto catalog=rss::save_directory(legacy,directory);rss::refresh_directory(legacy,catalog,cancel);
        rss::Feed original;original.url="https://EXAMPLE.com:443/7/rss#original";original.title="我的自定义名称";original.group="我的分组";
        const auto feed_id=legacy.add_feed(original);
        rss::Entry article;article.guid="keep-history";article.title="保留阅读记录";article.text="正文";
        rss::ParsedFeed parsed;parsed.entries.push_back(article);legacy.save_entries(feed_id,parsed);
        const auto entry=legacy.entries(rss::View::All).front().id;
        legacy.set_read(entry,true);legacy.set_starred(entry,true);legacy.set_position(entry,6);
        rss::App reopened(legacy,canvas);
        const auto click=[&](int x,int y){reopened.draw();reopened.tap(x,y);};
        click(100,1392);click(140,966);click(200,536);
        click(200,260);reopened.text_input("博客 7");reopened.submit();
        click(200,600);click(950,136);
        const auto feeds=legacy.feeds();
        assert(feeds.size()==1&&feeds.front().id==feed_id&&feeds.front().url==original.url);
        assert(feeds.front().title==original.title&&feeds.front().group==original.group);
        const auto history=legacy.entry(entry);assert(history&&history->read&&history->starred&&history->position==6);
        reopened.back();click(200,348);click(950,136);click(200,520);
        for(int i=0;i<2048;++i)reopened.backspace();reopened.text_input("https://example.com/7/rss");reopened.submit();
        bool duplicate=false;try{click(950,136);}catch(const std::runtime_error&){duplicate=true;}
        assert(duplicate&&legacy.feeds().size()==1);
    }
    std::cout<<"directory UI checks passed\n";
}
