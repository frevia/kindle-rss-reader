#include "app.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>

namespace {
std::string font_path(){
    if(const auto* font=std::getenv("RSS_FONT"))return font;
    if(std::filesystem::exists("/System/Library/Fonts/STHeiti Light.ttc"))return "/System/Library/Fonts/STHeiti Light.ttc";
    return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
}
std::int64_t add_feed(rss::Store& store){rss::Feed feed;feed.url="https://autosync.invalid/rss";return store.add_feed(feed);}
std::int64_t add_entry(rss::Store& store,std::int64_t feed,const std::string& guid,bool long_text=false){
    rss::Entry entry;entry.guid=guid;entry.title="自动同步测试文章 "+guid;entry.text="保持正文不变。\n\n第二段内容。";
    if(long_text)for(int i=0;i<100;++i)entry.text+="\n\n阅读位置不能因后台同步完成而改变，用户翻页后的页面仍然需要正常显示。";
    rss::ParsedFeed parsed;parsed.entries.push_back(entry);store.save_entries(feed,parsed);return store.entries(rss::View::All).front().id;
}
void wifi(rss::App& app){rss::ReaderStatus status;status.wifi=true;app.set_reader_status(status);}
}
int main(){
    const auto font=font_path();rss::Canvas canvas(1072,1448,font);constexpr std::time_t now=1800000000;
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);add_entry(store,feed,"first");
        rss::App app(store,canvas);wifi(app);app.maybe_auto_sync(now);assert(app.take_request()=="auto-sync"&&app.sync_state()==rss::App::SyncState::Running);
        assert(store.setting("auto_sync_last_attempt")==std::to_string(now));app.sync_finished(true);app.maybe_auto_sync(now+1799);assert(app.take_request().empty());
        rss::App reopened(store,canvas);wifi(reopened);reopened.maybe_auto_sync(now+1799);assert(reopened.take_request().empty());
        reopened.maybe_auto_sync(now+1800);assert(reopened.take_request()=="auto-sync");reopened.sync_finished(false,"network failure");
        reopened.maybe_auto_sync(now+3599);assert(reopened.take_request().empty());reopened.maybe_auto_sync(now+3600);assert(reopened.take_request()=="auto-sync");reopened.sync_finished(false);
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);add_entry(store,feed,"first");
        rss::App no_wifi(store,canvas);no_wifi.maybe_auto_sync(now);assert(no_wifi.take_request().empty());
        rss::App connected(store,canvas);wifi(connected);connected.maybe_auto_sync(now);assert(connected.take_request()=="auto-sync");connected.sync_finished(true);
        rss::Store empty(":memory:");empty.set_setting("reader_engine","classic");rss::App no_sources(empty,canvas);wifi(no_sources);no_sources.maybe_auto_sync(now);assert(no_sources.take_request().empty());
        store.set_setting("auto_sync","0");rss::App disabled(store,canvas);wifi(disabled);disabled.maybe_auto_sync(now);assert(disabled.take_request().empty());
        store.set_setting("auto_sync","1");store.set_setting("auto_sync_last_attempt",std::to_string(now));rss::App running(store,canvas);wifi(running);running.sync_started(true);running.maybe_auto_sync(now+2000);assert(running.take_request().empty());
        store.set_setting("auto_sync_last_attempt",std::to_string(now));rss::App manual(store,canvas);wifi(manual);manual.draw();manual.tap(100,1392);manual.draw();manual.tap(140,1174);manual.maybe_auto_sync(now+2000);assert(manual.take_request()=="sync");
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);add_entry(store,feed,"first");
        store.sync_result(feed,"","","",now-1000);rss::App recent(store,canvas);wifi(recent);recent.maybe_auto_sync(now);assert(recent.take_request().empty());
        store.sync_result(feed,"","","",now-1800);store.set_setting("auto_sync_last_attempt","not-a-time");rss::App invalid(store,canvas);wifi(invalid);invalid.maybe_auto_sync(now);assert(invalid.take_request()=="auto-sync");invalid.sync_finished(false);
        store.set_setting("auto_sync_last_attempt",std::to_string(now+9000));rss::App future(store,canvas);wifi(future);future.maybe_auto_sync(now+2000);assert(future.take_request()=="auto-sync");future.sync_finished(false);
        store.set_setting("auto_sync_last_attempt",std::to_string(now-5000));rss::App old(store,canvas);wifi(old);old.maybe_auto_sync(now);assert(old.take_request()=="auto-sync");old.sync_finished(false);
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);const auto original=add_entry(store,feed,"old",true);
        rss::App app(store,canvas);wifi(app);app.maybe_auto_sync(now);assert(app.take_request()=="auto-sync");app.open(original);app.next();const auto page=app.reader_page();assert(page>0);app.draw();const auto paper=canvas.pixels;assert(!app.dirty);
        const auto added=add_entry(store,feed,"new");app.sync_progress(1,1,0,"完成");assert(!app.dirty);app.sync_finished(false);assert(!app.dirty&&app.current_id()==original&&app.reader_page()==page);
        app.draw();assert(canvas.pixels==paper&&app.current_id()==original&&app.reader_page()==page);
        app.sync_started(false,true);app.next();assert(app.dirty&&app.reader_page()==page+1);
        app.sync_finished(false);assert(app.dirty&&app.reader_page()==page+1);app.draw();
        app.back();app.draw();app.tap(100,268);assert(app.current_id()==added);
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);add_entry(store,feed,"old");
        rss::App app(store,canvas);wifi(app);app.maybe_auto_sync(now);assert(app.take_request()=="auto-sync");
        app.draw();app.tap(990,160);app.text_input("自动同步");const auto added=add_entry(store,feed,"new");app.sync_finished(false);app.draw();
        app.submit();assert(app.query()=="自动同步");app.draw();app.tap(100,268);assert(app.current_id()==added);app.back();
        app.draw();app.tap(100,1392);app.draw();app.tap(140,1280);assert(app.screen()==rss::App::Screen::List);
        const auto confirmed=add_entry(store,feed,"confirmed");app.sync_started(false,true);app.sync_finished(false);app.draw();app.back();app.draw();app.tap(100,268);assert(app.current_id()==confirmed);
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");const auto feed=add_feed(store);add_entry(store,feed,"old");
        rss::App app(store,canvas);wifi(app);app.draw();app.tap(100,1392);app.draw();app.tap(140,1070);assert(app.screen()==rss::App::Screen::Settings);
        assert(store.setting("auto_sync").empty());
        app.draw();if(const auto* folder=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(folder);canvas.write_pgm((std::filesystem::path(folder)/"settings-auto-sync-on.pgm").string());}
        app.tap(950,630);assert(store.setting("auto_sync")=="0");app.draw();
        if(const auto* folder=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(folder)/"settings-auto-sync-off.pgm").string());
        app.back();rss::App reopened(store,canvas);wifi(reopened);reopened.maybe_auto_sync(1);assert(reopened.take_request().empty());
    }
}
