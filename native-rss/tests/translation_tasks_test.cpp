#define main kindle_rss_main
#define sync_feeds probe_sync_feeds
#define translate_selection probe_translate_selection
#define translate_article probe_translate_article
#define qwen_config probe_qwen_config
#include "../src/main.cpp"
#undef qwen_config
#undef translate_article
#undef translate_selection
#undef sync_feeds
#undef main
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
std::atomic<bool> sync_entered{false}, sync_cancelled{false}, sync_released{false}, sync_exited{false};
std::atomic<int> translation_calls{0};
}

namespace rss {
QwenConfig probe_qwen_config(const std::string&) {
    QwenConfig config;
    config.provider="qwen";config.model="qwen-mt-turbo";config.api_key="test-only";
    config.endpoint="http://127.0.0.1:1/test";
    return config;
}
void probe_sync_feeds(Store&,const std::atomic<bool>& cancel,
                      const std::function<void(const SyncProgress&)>& progress,bool) {
    sync_entered=true;
    progress({0,1,0,"mock sync"});
    while(!cancel)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sync_cancelled=true;
    while(!sync_released)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sync_exited=true;
}
std::string probe_translate_selection(Store&,const SelectionJob& job,const QwenConfig&,
                                      const std::atomic<bool>& cancel) {
    assert(sync_exited&&!cancel&&job.range.text=="English");
    ++translation_calls;
    return "英语";
}
void probe_translate_article(Store&,std::int64_t id,bool web,const QwenConfig&,
                             const std::atomic<bool>& cancel,
                             const std::function<void(const SyncProgress&)>& progress) {
    assert(sync_exited&&!cancel&&id>0&&!web);
    ++translation_calls;
    progress({1,1,0,"mock translation"});
}
}

namespace {
void run(bool selection,bool cancel_wait,bool quit_while_waiting=false) {
    sync_entered=false;sync_cancelled=false;sync_released=false;sync_exited=false;translation_calls=0;
    rss::Store store(":memory:");store.set_setting("reader_engine","classic");
    const auto feed_id=store.add_feed({0,"https://example.invalid/feed","test"});
    rss::Entry entry;entry.guid="one";entry.title="Test article";entry.url="https://example.invalid/story";
    entry.html="<p>English readers select useful words.</p>";entry.text="English readers select useful words.";
    rss::ParsedFeed parsed;parsed.entries={entry};store.save_entries(feed_id,parsed);
    const std::string fallback="/System/Library/Fonts/STHeiti Light.ttc";
    const std::string system_font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):fallback;
    if(!std::filesystem::exists(font))font=system_font;
    rss::Canvas canvas(1072,1448,font);
    rss::App app(store,canvas);
    UiTasks tasks(store,app,":memory:");
    app.set_reader_status({100,false,true,"12:00"});
    const auto until=[&](auto done) {
        for(int i=0;i<600&&!done();++i) {
            tasks.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(done());
    };
    const auto tap=[&](int x,int y) {app.draw();app.tap(x,y);tasks.tick();};
    until([]{return sync_entered.load();});
    const auto entry_id=store.entries(rss::View::All).front().id;
    app.open(entry_id);app.draw();
    rss::TouchPoint selection_point{};
    if(selection) {
        for(int y=110;y<550&&!app.selected_text();y+=5) {
            selection_point={42,y};
            rss::TouchGesture hold(selection_point);hold.phase=rss::TouchPhase::LongPress;hold.hold_ms=700;app.touch(hold);
        }
        assert(app.selected_text()&&app.selected_text()->range.text=="English");
        const auto rect=app.selected_text()->rects.front();
        const int top=96+rect.y-104<96?96+rect.y+rect.h+24:96+rect.y-104;
        tap(152,top+40);
    } else {
        tap(536,600);tap(536,1380);tap(180,1296);
    }
    assert(app.reader_panel()==(selection?rss::App::ReaderPanel::Selection:rss::App::ReaderPanel::Translation));
    until([]{return sync_cancelled.load();});
    assert(!sync_exited&&translation_calls==0);
    if(selection&&cancel_wait) {
        app.back();rss::TouchGesture hold(selection_point);
        hold.phase=rss::TouchPhase::LongPress;hold.hold_ms=700;app.touch(hold);app.draw();
        assert(app.selected_text());
        // Reopen the same selection. It must recover the queued cancel action.
        const auto point=app.selected_text()->rects.front();
        const int top=96+point.y-104<96?96+point.y+point.h+24:96+point.y-104;
        tap(152,top+40);tap(536,1304);
        assert(sync_cancelled&&!sync_exited&&translation_calls==0);
    } else if(cancel_wait) {
        tap(180,1296);
        assert(sync_cancelled&&!sync_exited&&translation_calls==0);
    }
    if(quit_while_waiting)app.quit=true;
    sync_released=true;
    if(!cancel_wait&&!quit_while_waiting)until([]{return translation_calls==1;});
    until([&]{return sync_exited&&app.sync_state()!=rss::App::SyncState::Running;});
    tasks.tick();
    assert(app.sync_state()!=rss::App::SyncState::Running);
    assert(translation_calls==(cancel_wait||quit_while_waiting?0:1));
}
}

int main() {
    for(bool selection:{true,false}) {
        run(selection,false);
        run(selection,true);
        run(selection,false,true);
    }
    std::cout<<"Automatic sync hands off to translation. Cancel, exit, and reopen-then-cancel passed.\n";
}
