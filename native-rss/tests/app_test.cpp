#include "app.hpp"
#include "feed.hpp"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>

int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Store store(":memory:");store.set_setting("reader_engine","classic");rss::Canvas canvas(1072,1448,font);
    for(const std::string engine:{"classic","cre"}){
        if(engine=="cre"&&!rss::CreReader::available())continue;
        const auto path=std::filesystem::temp_directory_path()/("rss-write-failure-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".sqlite");
        {
            rss::Store disk(path.string());disk.set_setting("reader_engine",engine);
            rss::Feed feed;feed.url="https://write-failure.invalid/feed";const auto fid=disk.add_feed(feed);
            rss::Entry entry;entry.guid="full";entry.title="写入失败也能返回";
            for(int i=0;i<100;++i)entry.html+="<p>数据库空间不足时，应保留旧进度并允许返回文章列表。</p>";
            entry.text=rss::html_to_text(entry.html);rss::ParsedFeed batch;batch.entries={entry};disk.save_entries(fid,batch);
            const auto id=disk.entries(rss::View::All).front().id;
            sqlite3* control=nullptr;assert(sqlite3_open(path.c_str(),&control)==SQLITE_OK);
            const auto block_writes=[&]{
                // Fail the real SQLite calls made by open/save_position, without filling the host disk.
                assert(sqlite3_exec(control,
                    "CREATE TRIGGER fail_entry BEFORE UPDATE ON entries BEGIN SELECT RAISE(ABORT,'database or disk is full'); END;"
                    "CREATE TRIGGER fail_setting BEFORE INSERT ON settings BEGIN SELECT RAISE(ABORT,'database or disk is full'); END;",
                    nullptr,nullptr,nullptr)==SQLITE_OK);
            };
            const auto allow_writes=[&]{assert(sqlite3_exec(control,"DROP TRIGGER fail_entry; DROP TRIGGER fail_setting;",nullptr,nullptr,nullptr)==SQLITE_OK);};
            rss::App reader(disk,canvas);reader.show_view(rss::View::All);reader.draw();block_writes();
            bool failed=false;
            try{reader.tap(150,244);}catch(const std::exception& e){failed=true;assert(std::string(e.what())=="database or disk is full");reader.show_error(e.what());}
            assert(failed&&!reader.reading());reader.draw();reader.back();reader.draw();assert(!reader.reading());
            assert(!disk.entry(id)->read&&disk.entry(id)->position==0);
            allow_writes();
            disk.set_setting("last_entry","previous");
            for(const std::string phase:{"position","content","last"}){
                // Earlier writes succeed before this failure; the whole open must still roll back.
                const std::string trigger=phase=="position"?
                    "CREATE TRIGGER fail_open BEFORE UPDATE OF position ON entries BEGIN SELECT RAISE(ABORT,'database or disk is full'); END;":
                    "CREATE TRIGGER fail_open BEFORE INSERT ON settings WHEN NEW.key='"+
                        (phase=="content"?"reader_content:"+std::to_string(id):"last_entry")+
                        "' BEGIN SELECT RAISE(ABORT,'database or disk is full'); END;";
                assert(sqlite3_exec(control,trigger.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
                failed=false;
                try{reader.open(id);}catch(const std::exception& e){failed=true;assert(std::string(e.what())=="database or disk is full");reader.show_error(e.what());}
                assert(failed&&!reader.reading());reader.back();reader.reload();reader.draw();
                const auto retained=disk.entry(id);
                assert(!retained->read&&!retained->in_progress&&retained->position==0);
                assert(disk.entry_count(rss::View::Unread)==1&&disk.entries(rss::View::Unread).size()==1);
                assert(disk.setting("reader_content:"+std::to_string(id)).empty());
                assert(disk.setting("last_entry")=="previous");
                assert(sqlite3_exec(control,"DROP TRIGGER fail_open",nullptr,nullptr,nullptr)==SQLITE_OK);
            }
            reader.open(id);
            assert(disk.entry(id)->read&&disk.entry(id)->in_progress);
            assert(disk.setting("reader_content:"+std::to_string(id))=="rss"&&disk.setting("last_entry")==std::to_string(id));
            reader.next();reader.draw();assert(reader.reader_page()>0);
            const auto saved_position=disk.entry(id)->position;
            const auto saved_anchor=disk.setting("reader_cre:"+std::to_string(id));
            block_writes();reader.back();assert(!reader.reading());reader.draw();
            assert(disk.entry(id)->position==saved_position&&disk.setting("reader_cre:"+std::to_string(id))==saved_anchor);
            reader.tap(100,1392);assert(reader.menu_open());reader.back();
            allow_writes();reader.open(id);assert(reader.reader_page()>0);reader.back();assert(!reader.reading());
            assert(sqlite3_close(control)==SQLITE_OK);
        }
        std::filesystem::remove(path);
    }
    {
        rss::Store opened(":memory:");opened.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://home-entry.test/rss";const auto fid=opened.add_feed(feed);
        rss::ParsedFeed batch;
        for(int i=0;i<2;++i){rss::Entry item;item.guid=std::to_string(i);item.title="首页文章 "+std::to_string(i);item.text="正文内容";batch.entries.push_back(item);}
        opened.save_entries(fid,batch);const auto entries=opened.entries(rss::View::Unread);
        rss::App direct(opened,canvas,entries.back().id);
        assert(direct.reading()&&direct.current_id()==entries.back().id);
        assert(opened.entry(entries.back().id)->read);
        assert(!opened.entry(entries.front().id)->read);
        direct.draw();direct.back();assert(!direct.reading());
        direct.draw();
        rss::Canvas expected_count(1072,1448,font);
        const std::string count_label="第 1 / 1 页 · 共 1 篇";
        expected_count.text(536-expected_count.measure(count_label,24)/2,1406,count_label,24,90);
        for(int y=1348;y<1448;++y)for(int x=336;x<712;++x)
            assert(canvas.pixels[y*1072+x]==expected_count.pixels[y*1072+x]);

        assert(opened.entries(rss::View::Unread).size()==1);
        rss::App missing(opened,canvas,999999);assert(!missing.reading());
        missing.draw();missing.back();missing.draw();
        rss::App normal(opened,canvas);assert(!normal.reading());
    }
    {
        rss::Store preferences(":memory:");preferences.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://home-settings.test/rss";const auto fid=preferences.add_feed(feed);
        rss::Entry entry;entry.guid="untouched";entry.title="保留的未读文章";entry.text="保留的正文";
        rss::ParsedFeed batch;batch.entries={entry};preferences.save_entries(fid,batch);
        rss::App settings(preferences,canvas);settings.show_view(rss::View::All,fid);
        auto touch=[&](int x,int y){settings.draw();settings.tap(x,y);};
        auto shot=[&](const char* name){settings.draw();if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(folder);canvas.write_pgm((std::filesystem::path(folder)/name).string());}};
        touch(100,1392);touch(140,1070);assert(settings.screen()==rss::App::Screen::Settings&&!settings.menu_open());
        assert(preferences.setting("home_visible").empty());shot("settings-home-on.pgm");const auto on=canvas.pixels;
        touch(950,326);assert(preferences.setting("home_visible")=="0");shot("settings-home-off.pgm");const auto off=canvas.pixels;assert(off!=on);
        settings.back();assert(settings.screen()==rss::App::Screen::List);settings.draw();const auto list=canvas.pixels;
        {rss::App reopened(preferences,canvas);reopened.draw();reopened.tap(100,1392);reopened.draw();reopened.tap(140,1070);reopened.draw();assert(canvas.pixels==off);}
        touch(100,1392);touch(140,1070);touch(200,326);assert(preferences.setting("home_visible")=="1");
        settings.back();settings.draw();assert(canvas.pixels==list);
        assert(preferences.entry_count(rss::View::Unread)==1&&!preferences.entries(rss::View::All).front().read);
        assert(settings.take_request().empty());
    }
    {
        // Hiding sync preserves the filtered page; the overlay consumes underlying article taps.
        rss::Store background(":memory:");background.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://sync-dialog.test/rss";feed.title="后台测试";feed.group="博客";
        const auto fid=background.add_feed(feed);rss::ParsedFeed batch;
        for(int i=0;i<12;++i){rss::Entry e;e.guid=std::to_string(i);e.title="同步测试 "+std::to_string(i);e.text="正文";batch.entries.push_back(e);}
        background.save_entries(fid,batch);rss::App dialog(background,canvas);
        auto touch=[&](int x,int y){dialog.draw();dialog.tap(x,y);};
        dialog.show_group("博客");touch(990,140);dialog.text_input("同步测试");dialog.submit();dialog.next();dialog.draw();const auto list=canvas.pixels;
        touch(100,1392);touch(140,1174);assert(dialog.take_request()=="sync");dialog.draw();
        for(int y=0;y<1448;++y)for(int x=0;x<1072;++x)if(x<48||x>=1024||y<484||y>=964)assert(canvas.pixels[y*1072+x]==(y<76?list[y*1072+x]:192+list[y*1072+x]/4));
        touch(150,350);dialog.next();assert(!dialog.reading()&&dialog.screen()==rss::App::Screen::Sync);
        touch(800,904);assert(dialog.screen()==rss::App::Screen::List&&dialog.sync_state()==rss::App::SyncState::Running&&dialog.take_request().empty());
        dialog.draw();assert(canvas.pixels==list);
        dialog.sync_progress(1,2,0,"正在更新第二个订阅");assert(!dialog.dirty);
        touch(100,1392);touch(140,1174);assert(dialog.take_request().empty());
        touch(200,904);assert(dialog.take_request()=="cancel-sync");dialog.sync_finished(true);
        touch(800,904);assert(dialog.screen()==rss::App::Screen::List&&dialog.query()=="同步测试"&&dialog.selected_group()==std::optional<std::string>("博客"));
        touch(100,1392);touch(140,1174);assert(dialog.take_request()=="sync");touch(800,904);
        touch(150,350);assert(dialog.reading());const auto id=dialog.current_id();const auto page=dialog.reader_page();
        dialog.draw();dialog.sync_progress(2,2,0,"更新完成");assert(!dialog.dirty);dialog.sync_finished(false);
        assert(dialog.reading()&&dialog.current_id()==id&&dialog.reader_page()==page&&dialog.sync_state()==rss::App::SyncState::Complete);
    }
    {
        rss::Store compact(":memory:");compact.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://compact-list.test/rss";feed.title="博客来源";feed.group="博客";
        const auto fid=compact.add_feed(feed);rss::ParsedFeed batch;
        for(int i=0;i<14;++i){rss::Entry e;e.guid=std::to_string(i);e.title=i%2?"适当留白，让阅读更轻松":"Kindle typography "+std::to_string(i);e.text="正文";e.published=i+1;batch.entries.push_back(e);}
        compact.save_entries(fid,batch);rss::App list(compact,canvas);list.show_view(rss::View::All);list.draw();const auto paper=canvas.pixels;
        const auto entries=compact.entries(rss::View::All);
        for(int row=0;row<7;++row){
            const int top=164+row*160,bottom=top+159;int first=bottom,last=top;
            for(int y=top+1;y<bottom;++y)for(int x=76;x<1040;++x)if(paper[y*1072+x]<160){first=std::min(first,y);last=std::max(last,y);}
            assert(first<last&&std::abs((first-top)-(bottom-last))<=3);
            list.draw();list.tap(100,top+80);assert(list.current_id()==entries[row].id);list.back();
        }
        if(auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(dir);list.draw();canvas.write_pgm((std::filesystem::path(dir)/"list-seven-centered.pgm").string());}
        list.next();list.draw();list.tap(100,268);assert(list.current_id()==entries[7].id);
    }
    const auto expect_pager=[&](const std::vector<std::uint8_t>& paper,int page,int total,int count=24){
        rss::Canvas footer(1072,1448,font);
        const auto label="第 "+std::to_string(page)+" / "+std::to_string(total)+" 页 · 共 "+std::to_string(count)+" 篇";
        footer.text(536-footer.measure(label,24)/2,1406,label,24,90);
        for(int y=1348;y<1448;++y)for(int x=336;x<712;++x)
            assert(paper[y*1072+x]==footer.pixels[y*1072+x]);
    };
    {
        rss::Store totals(":memory:");totals.set_setting("reader_engine","classic");rss::Feed source;source.url="https://page-total.test/rss";source.title="分页测试";source.group="目录";
        const auto fid=totals.add_feed(source);rss::ParsedFeed batch;
        for(int i=0;i<24;++i){rss::Entry item;item.guid=std::to_string(i);item.title="测试文章 "+std::to_string(i);item.text="正文";batch.entries.push_back(item);}
        totals.save_entries(fid,batch);rss::App pager(totals,canvas);
        pager.draw();expect_pager(canvas.pixels,1,4); // Exact total is available before visiting later pages.
        pager.next();pager.next();pager.draw();expect_pager(canvas.pixels,3,4);
        pager.notify("同步已完成，点击更新列表");pager.draw();expect_pager(canvas.pixels,3,4);
        pager.tap(100,1392);pager.draw();pager.tap(140,1174);assert(pager.take_request()=="sync");pager.back();pager.draw();expect_pager(canvas.pixels,3,4);
        pager.sync_finished(true);pager.draw();expect_pager(canvas.pixels,3,4);
        pager.tap(200,1288);pager.draw();expect_pager(canvas.pixels,3,4);
        pager.show_group("目录");pager.draw();expect_pager(canvas.pixels,1,4);
        pager.show_view(rss::View::All);pager.draw();pager.tap(990,160);pager.text_input("没有匹配");pager.submit();pager.draw();expect_pager(canvas.pixels,1,1,0);
        for(const auto& entry:totals.entries(rss::View::All))totals.set_read(entry.id,true);
        pager.show_view(rss::View::Unread);pager.draw();expect_pager(canvas.pixels,1,1,0);
        pager.show_view(rss::View::All);pager.draw();expect_pager(canvas.pixels,1,4);
        if(auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(dir);canvas.write_pgm((std::filesystem::path(dir)/"list-exact-page-total.pgm").string());}
        source.id=fid;source.title.clear();for(int i=0;i<140;++i)source.title+="长";
        totals.update_feed(source);pager.reload();pager.draw();expect_pager(canvas.pixels,1,4);
        source.title="分页测试";totals.update_feed(source);pager.reload();pager.draw();expect_pager(canvas.pixels,1,4);
        for(auto& entry:batch.entries){entry.title.clear();for(int i=0;i<100;++i)entry.title+="长";}
        totals.save_entries(fid,batch);pager.reload();pager.draw();const auto changed_titles=canvas.pixels;
        rss::App fresh(totals,canvas);fresh.show_view(rss::View::All);fresh.draw();assert(canvas.pixels==changed_titles);
    }
    {
        rss::Store pages_store(":memory:");pages_store.set_setting("reader_engine","classic");rss::Feed feed;feed.url="https://pages.example/feed";
        const auto feed_id=pages_store.add_feed(feed);rss::ParsedFeed batch;
        for(int i=0;i<45;++i){rss::Entry item;item.guid=std::to_string(i);
            item.title=i%3==0?std::string(110,'x'):"文章 "+std::to_string(i);item.text="正文";batch.entries.push_back(item);}
        pages_store.save_entries(feed_id,batch);const auto ordered=pages_store.entries(rss::View::All,feed_id);
        rss::App pages(pages_store,canvas);pages.show_view(rss::View::All,feed_id);
        auto scan_page=[&]{
            std::vector<std::int64_t> found;
            for(int y=180;y<1335;y+=40){
                pages.draw();pages.tap(100,y);
                if(!pages.reading())continue;
                found.push_back(pages.current_id());pages.back();
            }
            std::vector<std::int64_t> unique;
            for(auto id:found)if(unique.empty()||unique.back()!=id)unique.push_back(id);
            return unique;
        };
        std::vector<std::vector<std::int64_t>> seen_pages;
        std::set<std::int64_t> seen;
        pages.draw();const auto first_list_page=canvas.pixels;
        auto visible=scan_page();
        for(;;){
            assert(!visible.empty());
            for(auto id:visible)assert(seen.insert(id).second);
            seen_pages.push_back(visible);
            pages.next();
            auto candidate=scan_page();
            if(candidate==visible)break;
            visible=std::move(candidate);
        }
        pages.draw();pages.tap(970,1392);assert(scan_page()==seen_pages.back()&&!pages.menu_open());
        assert(seen.size()==ordered.size());
        expect_pager(first_list_page,1,static_cast<int>(seen_pages.size()),45);
        for(std::size_t p=seen_pages.size()-1;p>0;--p){pages.previous();assert(scan_page().front()==seen_pages[p-1].front());}
        pages.previous();assert(scan_page().front()==ordered.front().id);
        // Navigation may arrive before the next draw, and reload preserves the ordinal.
        pages.next();pages.next();assert(scan_page().front()==seen_pages[2].front());auto current=scan_page().front();
        pages.reload();assert(scan_page().front()==current);
        // The on-screen pager has distinct targets; changing a filter resets its page.
        pages.draw();pages.tap(900,1380);assert(scan_page().front()==seen_pages[3].front());
        pages.draw();pages.tap(800,1380);assert(scan_page().front()==current);
        pages.draw();pages.tap(750,160);assert(scan_page().front()==ordered.front().id);
        pages.draw();pages.tap(800,1380);assert(scan_page().front()==ordered.front().id);
        pages.next();pages.next();
        // Shrinking the snapshot clamps to the remaining page, then to the empty page.
        pages_store.set_starred(ordered.back().id,true);pages_store.remove_feed(feed_id);
        pages.reload();assert(scan_page().front()==ordered.back().id);
        pages.next();assert(scan_page().front()==ordered.back().id);
        pages_store.set_starred(ordered.back().id,false);pages_store.remove_feed(feed_id);
        pages.reload();pages.next();pages.previous();pages.draw();pages.tap(100,300);assert(!pages.reading());
        assert(ordered.size()==45);
    }
    {
        rss::Store scoped(":memory:");scoped.set_setting("reader_engine","classic");rss::Feed local;local.url="https://scope.example/feed";local.title="本地源";local.group="科技";
        const auto local_id=scoped.add_feed(local);rss::Feed remote;remote.url="https://other.example/feed";remote.title="其他源";
        const auto remote_id=scoped.add_feed(remote);
        rss::Entry a;a.guid="local";a.title="needle 本地文章";a.text="正文";a.published=100;
        rss::Entry b;b.guid="remote";b.title="远端更新";b.text="正文";b.published=200;
        auto extra=a;extra.guid="unrelated";extra.title="不匹配搜索的较新文章";extra.published=300;
        rss::ParsedFeed local_batch;local_batch.entries={a,extra};scoped.save_entries(local_id,local_batch);
        rss::ParsedFeed remote_batch;remote_batch.entries={b};scoped.save_entries(remote_id,remote_batch);
        const auto local_entry=scoped.entries(rss::View::All,local_id,"needle").front().id;
        rss::App filtered(scoped,canvas);filtered.show_view(rss::View::All,local_id);
        auto scope_tap=[&](int x,int y){filtered.draw();filtered.tap(x,y);};
        scope_tap(990,160);filtered.text_input("needle");filtered.submit();
        scope_tap(638,160); // Changing a filter retains the keyword and feed scope.
        scope_tap(100,300);assert(filtered.current_id()==local_entry);
        filtered.back();
        scoped.set_starred(local_entry,true);
        scoped.set_starred(scoped.entries(rss::View::All,remote_id).front().id,true);
        filtered.show_group("科技");scope_tap(870,160);scope_tap(100,300);
        assert(filtered.current_id()==local_entry);
    }
    {
        rss::Store room(":memory:");room.set_setting("reader_engine","classic");rss::Feed feed;feed.url="https://room.example/feed";
        const auto fid=room.add_feed(feed);rss::Entry story;story.guid="room";story.title="正文空间";
        for(int i=0;i<100;++i)story.text+="正文\n";
        rss::ParsedFeed batch;batch.entries={story};room.save_entries(fid,batch);
        const auto eid=room.entries(rss::View::All).front().id;rss::App reader(room,canvas);reader.open(eid);
        reader.draw();reader.tap(900,1380);assert(reader.reader_page()==1);
        assert(room.entry(eid)->position>=static_cast<std::int64_t>(story.title.size()+2+14*std::string("正文\n").size()));
        reader.draw();reader.tap(180,1380);assert(reader.reader_page()==0);
        reader.draw();reader.tap(100,55);assert(!reader.reading());
        reader.open(eid);
        for(int i=0;i<100;++i){const auto at=reader.reader_page();reader.next();if(reader.reader_page()==at)break;}
        reader.draw();reader.tap(900,1420);assert(!reader.reading()); // Explicit completion stays in the compact footer.
    }
    {
        rss::Store preferences(":memory:");preferences.set_setting("reader_engine","classic");rss::Feed feed;feed.url="https://prefs.example/feed";
        const auto fid=preferences.add_feed(feed);rss::Entry story;story.guid="saved";story.title="已保存排版";story.text=std::string(3000,'x');
        rss::ParsedFeed batch;batch.entries={story};preferences.save_entries(fid,batch);
        preferences.set_setting("font_size","30");preferences.set_setting("line_gap","18");preferences.set_setting("margin","60");
        const auto eid=preferences.entries(rss::View::All)[0].id;rss::App reader(preferences,canvas);reader.open(eid);
        assert(preferences.setting("font_size")=="30"&&preferences.setting("line_gap")=="18"&&preferences.setting("margin")=="60");
        reader.next();const auto old_position=preferences.entry(eid)->position;
        reader.draw();reader.tap(530,400);reader.draw();assert(reader.reader_tools_open());
        const auto menu_page=reader.reader_page();reader.tap(178,1380);reader.draw();
        assert(reader.reader_panel()==rss::App::ReaderPanel::Type);
        reader.tap(950,400);assert(reader.reader_page()==menu_page); // Visible text closes the panel without turning the page.
        assert(!reader.reader_tools_open()&&reader.reader_panel()==rss::App::ReaderPanel::None);
        reader.draw();reader.tap(530,400);reader.draw();reader.tap(178,1380);reader.draw();reader.tap(960,856);
        assert(preferences.setting("font_size")=="32"&&preferences.setting("line_gap")=="18"&&preferences.setting("margin")=="60");
        const auto new_position=preferences.entry(eid)->position;
        assert(new_position<=old_position);
    }
    rss::Feed f;f.url="https://example.com/feed";f.title="测试来源";auto id=store.add_feed(f);
    rss::Entry e;e.guid="one";e.title="中文文章标题";
    for(int i=0;i<200;++i)e.text+="阅读位置必须跨字号保留，返回列表不应自动重排。\n";
    rss::ParsedFeed parsed;parsed.entries={e};store.save_entries(id,parsed);
    auto entry=store.entries(rss::View::All)[0];rss::App app(store,canvas);
    auto snapshot=[&](const char* name){
        app.draw();
        if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){
            std::filesystem::create_directories(folder);
            canvas.write_pgm((std::filesystem::path(folder)/name).string());
        }
    };
    snapshot("list.pgm");
    // Sync is only reachable through the brand menu; its former target is inert.
    for(int y=30;y<90;++y)for(int x=1040;x<1072;++x)assert(canvas.pixels[y*1072+x]==255);
    app.tap(960,64);assert(app.take_request().empty());app.tap(100,1392);app.draw();app.tap(140,1174);assert(app.take_request()=="sync");app.sync_finished(true);app.back();
    app.draw();app.tap(100,1392);app.draw();app.tap(120,966);app.draw();snapshot("navigation.pgm");app.back();
    app.draw();app.tap(100,300);assert(app.reading());assert(store.entry(entry.id)->read);
    snapshot("reader.pgm");
    app.next();app.next();assert(app.reader_page()==2);const auto position=store.entry(entry.id)->position;assert(position>0);
    app.back();assert(!app.reading());app.draw();app.tap(100,300);assert(app.reading());assert(app.reader_page()==2);
    app.draw();app.tap(530,400);snapshot("reader-tools.pgm");
    app.tap(178,1380);snapshot("reader-type.pgm");app.tap(960,856);app.draw(); // Increase font in the type panel.
    assert(app.reading()&&app.reader_panel()==rss::App::ReaderPanel::Type);app.back();app.back();app.back();app.show_view(rss::View::Unread);app.draw();
    assert(store.entries(rss::View::Unread).empty());
    app.show_view(rss::View::All);app.draw();app.tap(100,300);app.draw();
    const auto page_before_star=app.reader_page();app.tap(950,50);assert(!store.entry(entry.id)->starred);
    app.draw();app.tap(536,600);app.draw();app.tap(950,50);assert(store.entry(entry.id)->starred&&app.reader_page()==page_before_star);
    app.back();app.back();app.show_view(rss::View::Starred);app.draw();
    assert(store.entries(rss::View::Starred).size()==1);
    // Navigation and draft edits preserve the confirmed data until save.
    auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
    auto manage=[&]{
        tap(100,1392);tap(120,966);
        const auto group=store.feeds().front().group;const int headers=group.empty()?1:1+static_cast<int>(std::count(group.begin(),group.end(),'/'));
        tap(950,250+headers*104+44);
    };
    manage();snapshot("subscription.pgm");tap(200,1124);snapshot("unsubscribe.pgm");tap(200,1380);
    assert(store.feeds().size()==1);app.back();app.back();
    store.set_read(entry.id,false);tap(100,300);tap(536,600);tap(894,1380);tap(536,1296);
    assert(!store.entry(entry.id)->read);
    app.back();app.back();app.back();manage();tap(800,1124);tap(900,1380);
    assert(store.entry(entry.id)->read);app.back();app.back();
    manage();tap(200,1124);tap(900,1380);assert(store.feeds().empty());assert(store.entry(entry.id)->starred);app.back();
    rss::Feed other;other.url="https://example.com/other";other.title="Other";
    const auto other_id=store.add_feed(other);rss::ParsedFeed old;rss::Entry clean;clean.guid="clean";clean.title="Old";clean.text="text";clean.published=100;
    old.entries={clean};store.save_entries(other_id,old);const auto clean_id=store.entries(rss::View::All,other_id)[0].id;
    store.set_read(clean_id,true);app.reload();
    tap(100,1392);tap(140,1174);app.take_request();app.sync_finished(true);tap(880,548);tap(200,1380);tap(200,1380);
    assert(store.entry(clean_id));tap(200,1380);tap(900,1380);assert(!store.entry(clean_id));assert(store.entry(entry.id));
    app.back();app.back();manage();tap(200,330);
    for(int i=0;i<5;++i)app.backspace();app.text_input("新的名称");app.submit();assert(store.feeds()[0].title=="Other");tap(950,1380);assert(store.feeds()[0].title=="新的名称");
    store.add_group("科技/编程");app.reload();app.back();manage();tap(400,770);tap(300,530);tap(950,1380);assert(store.feeds()[0].group=="科技/编程");
    app.back();manage();tap(200,330);app.text_input("取消修改");app.back();app.back();assert(store.feeds()[0].title=="新的名称");app.back();
    tap(100,1392);tap(120,966);tap(150,1380);snapshot("groups.pgm");app.back();app.back();app.show_group("科技");
    old.entries[0].guid="new-child";
    rss::ArticleImage photo;photo.url="https://example.test/photo.png";photo.alt="离线图片";
    rss::ArticleImage missing;missing.url="https://example.test/missing.png";
    old.entries[0].images={photo,missing};store.save_entries(other_id,old);app.reload();
    tap(100,300);assert(app.current_id()==store.entries(rss::View::All,other_id)[0].id);
    photo.width=320;photo.height=180;photo.pixels.resize(320*180);
    for(int y=0;y<180;++y)for(int x=0;x<320;++x)photo.pixels[y*320+x]=x%256;
    store.save_image(app.current_id(),photo);
    tap(536,600);tap(660,1380);app.draw();assert(app.reader_panel()==rss::App::ReaderPanel::Images);
    assert(canvas.pixels[950*1072+536]==160);snapshot("reader-images.pgm");
    app.next();snapshot("reader-images-uncached.pgm");app.back();assert(app.reading());
    app.back();
    const auto current=app.current_id();
    app.show_error("网络暂时不可用，已有文章仍可阅读");
    tap(100,300);assert(app.current_id()==current);app.back();assert(app.reading());
    // An image-only article must reopen the exact image page despite identical text offsets.
    {
        rss::Store image_store(":memory:");image_store.set_setting("reader_engine","classic");rss::Feed feed;feed.url="https://example.test/rss";
        const auto fid=image_store.add_feed(feed);
        rss::Entry story;story.guid="photos";story.title="图文阅读";story.url="https://example.test/post";
        story.html="<img src='/one'><img src='/two'><img src='/one'>";
        story.text=rss::html_to_text(story.html);story.images=rss::html_images(story.html,story.url);
        rss::ParsedFeed pf;pf.entries={story};image_store.save_entries(fid,pf);
        const auto eid=image_store.entries(rss::View::All)[0].id;
        for(auto image:story.images){image.width=600;image.height=900;image.pixels.assign(600*900,image.url.find("two")!=std::string::npos?155:77);image_store.save_image(eid,image);}
        rss::App reader(image_store,canvas);reader.open(eid);reader.draw();
        assert(canvas.pixels[400*1072+536]==77);
        reader.next();reader.next();assert(reader.reader_page()==2);reader.back();reader.open(eid);assert(reader.reader_page()==2);
        reader.draw();reader.tap(536,400);reader.draw();assert(reader.reader_tools_open());
        reader.tap(660,1380);reader.draw();assert(reader.reader_panel()==rss::App::ReaderPanel::Images);
        if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(folder)/"reader-images-multipage.pgm").string());
        reader.back();reader.back();assert(reader.reader_page()==2);
        reader.previous();assert(reader.reader_page()==1);reader.draw();reader.tap(536,400);reader.draw();reader.tap(660,1380);reader.draw();
        assert(reader.reader_panel()==rss::App::ReaderPanel::Images);assert(canvas.pixels[950*1072+536]==155);
        reader.back();reader.back();reader.draw();reader.tap(536,400);reader.draw();reader.tap(178,1380);reader.draw();reader.tap(960,856);
        assert(image_store.setting("font_size")=="36");reader.back();reader.back();
        reader.back();reader.open(eid);assert(reader.reader_page()==1);
    }
    {
        // Reader overlays replace the footer, isolate touches, and keep a stable live typography anchor.
        rss::Store controls(":memory:");controls.set_setting("reader_engine","classic");rss::Feed feed;feed.url="https://controls.example/rss";feed.title="独立界面样例";
        const auto fid=controls.add_feed(feed);rss::Entry story;story.guid="controls";
        story.title="当工具退后一步，文字就有了自己的速度";
        for(int i=0;i<5;++i)story.title+="：长标题在正文顶栏限行，在信息面板完整阅读";
        story.url="https://example.com/"+std::string(1700,'a')+"/完整原文地址";
        for(int i=0;i<240;++i)story.text+="第 "+std::to_string(i)+" 段：调整字号、行距和页边距时，应该继续看到原来的阅读位置。\n";
        rss::ParsedFeed batch;batch.entries={story};controls.save_entries(fid,batch);
        const auto eid=controls.entries(rss::View::All).front().id;controls.set_setting("margin","78");rss::App reader(controls,canvas);reader.open(eid);
        auto touch=[&](int x,int y){reader.draw();reader.tap(x,y);};
        auto capture=[&](const char* name){reader.draw();if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(folder)/name).string());};
        reader.set_reader_status({21,false,true,"09:19"});
        reader.next();reader.next();reader.draw();const auto normal=canvas.pixels;
        reader.dirty=false;reader.set_reader_status({21,false,true,"09:19"});assert(!reader.dirty);
        const auto saved=controls.entry(eid)->position;const auto saved_page=reader.reader_page();
        reader.set_reader_status({20,true,false,"09:20"});assert(reader.dirty);reader.draw();
        assert(reader.reader_page()==saved_page&&controls.entry(eid)->position==saved);
        assert(std::equal(normal.begin()+96*1072,normal.begin()+1376*1072,canvas.pixels.begin()+96*1072));
        assert(canvas.pixels!=normal);
        reader.set_reader_status({21,false,true,"09:19"});reader.draw();
        const auto page=reader.reader_page(),position=static_cast<std::size_t>(controls.entry(eid)->position);
        touch(536,600);capture("reader-tools.pgm");assert(reader.reader_tools_open());
        assert(reader.reader_page()==page&&controls.entry(eid)->position==static_cast<std::int64_t>(position));
        assert(std::equal(normal.begin()+96*1072,normal.begin()+700*1072,canvas.pixels.begin()+96*1072));
        touch(32,1380);assert(reader.reader_tools_open()); // Old previous-page target cannot turn the article.
        assert(reader.reader_panel()==rss::App::ReaderPanel::Type);capture("reader-type.pgm");
        reader.next();reader.previous();assert(reader.reader_page()==page);
        touch(20,900);assert(reader.reader_panel()==rss::App::ReaderPanel::Type&&reader.reader_page()==page);
        for(int i=0;i<10;++i)touch(960,856);
        assert(controls.setting("font_size")=="48");assert(controls.entry(eid)->position==static_cast<std::int64_t>(position));
        for(int i=0;i<15;++i)touch(640,856);
        assert(controls.setting("font_size")=="24");assert(controls.entry(eid)->position==static_cast<std::int64_t>(position));
        touch(960,962);touch(640,1066);
        assert(controls.setting("line_gap")=="32"&&controls.setting("margin")=="32");
        assert(controls.entry(eid)->position==static_cast<std::int64_t>(position)&&reader.reader_page()>0);
        touch(868,1066);assert(controls.setting("margin")=="78"&&controls.entry(eid)->position==static_cast<std::int64_t>(position));
        touch(640,1066);assert(controls.setting("margin")=="32"&&controls.entry(eid)->position==static_cast<std::int64_t>(position));
        {rss::App restored(controls,canvas);restored.open(eid,false);restored.draw();bool ink_at_narrow_edge=false;for(int y=110;y<150;++y)for(int x=32;x<40;++x)if(canvas.pixels[y*1072+x]<100)ink_at_narrow_edge=true;assert(ink_at_narrow_edge);}
        touch(660,1380);assert(reader.reader_panel()==rss::App::ReaderPanel::Images);capture("reader-images-empty.pgm");
        touch(900,1380);assert(reader.reader_panel()==rss::App::ReaderPanel::Info);capture("reader-info.pgm");
        const auto first=canvas.pixels;bool reached_last=false;
        for(int i=0;i<30;++i){const auto before=canvas.pixels;touch(950,1296);reader.draw();if(canvas.pixels==before){reached_last=true;break;}}
        assert(reached_last&&canvas.pixels!=first);capture("reader-info-last.pgm");
        bool reached_first=false;
        for(int i=0;i<30;++i){touch(100,1296);reader.draw();if(canvas.pixels==first){reached_first=true;break;}}
        assert(reached_first); // A short final information page must still navigate back to the first.
        const auto info_page=reader.reader_page();touch(536,1296);assert(!controls.entry(eid)->read&&reader.reader_page()==info_page);
        touch(950,790);assert(reader.reader_panel()==rss::App::ReaderPanel::None&&reader.reader_tools_open());
        touch(536,500);assert(!reader.reader_tools_open());
        const auto old=reader.reader_page();touch(950,1380);assert(reader.reader_page()==old+1);
        touch(536,600);touch(950,50);assert(controls.entry(eid)->starred);
        reader.back();reader.back();reader.draw();capture("list-starred.pgm");
    }
    std::cout<<"app checks passed\n";
    // Touch-only Chinese input: compose, correct, select, page candidates, and commit a feed name.
    rss::Store typing_store(":memory:");typing_store.set_setting("reader_engine","classic");rss::Feed typing_feed;typing_feed.url="https://example.test/rss";typing_feed.title="旧名";
    const auto typing_id=typing_store.add_feed(typing_feed);rss::App typing(typing_store,canvas);
    auto touch=[&](int x,int y){typing.draw();typing.tap(x,y);};
    auto keys=[&](const std::string& value){
        const std::vector<std::string> rows={"qwertyuiop","asdfghjkl","zxcvbnm"};
        for(char ch:value){bool found=false;
            for(std::size_t r=0;r<rows.size();++r){const auto column=rows[r].find(ch);if(column!=std::string::npos){
                const int start=(1072-static_cast<int>(rows[r].size())*95-(static_cast<int>(rows[r].size())-1)*6)/2;
                touch(start+static_cast<int>(column)*101+47,1016+static_cast<int>(r)*116);found=true;break;}}
            assert(found);
        }
    };
    auto edit_title=[&]{touch(100,1392);touch(120,966);touch(950,398);touch(200,330);};
    auto commit=[&]{touch(900,1364);touch(950,1380);typing.back();};
    edit_title();typing.backspace();typing.backspace();keys("kejix");typing.backspace();
    typing.submit();assert(typing_store.feeds()[0].title=="旧名");typing.back();
    if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){typing.draw();canvas.write_pgm((std::filesystem::path(folder)/"pinyin.pgm").string());canvas.write_pgm((std::filesystem::path(folder)/"candidates.pgm").string());}
    touch(180,642);commit();assert(typing_store.feeds()[0].title=="科技");
    edit_title();typing.backspace();typing.backspace();keys("shi");touch(900,836);touch(180,642);commit();assert(typing_store.feeds()[0].title!="科技");assert(!typing_store.feeds()[0].title.empty());
    edit_title();typing.backspace();keys("ni");touch(712,1364);keys("rss");commit();assert(typing_store.feeds()[0].title=="nirss");assert(typing_store.feeds()[0].id==typing_id);
    edit_title();for(int i=0;i<5;++i)typing.backspace();touch(100,1364);touch(131,1132);commit();assert(typing_store.feeds()[0].title=="@");
    edit_title();typing.backspace();keys("ni");touch(100,1248);keys("keji");touch(400,1364);touch(968,1248);commit();assert(typing_store.feeds()[0].title=="科");
    edit_title();typing.backspace();touch(712,1364);touch(100,1248);keys("rss");touch(968,1248);touch(100,1248);keys("s");
    if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){typing.draw();canvas.write_pgm((std::filesystem::path(folder)/"keyboard-english.pgm").string());}
    commit();assert(typing_store.feeds()[0].title=="RSs");
    edit_title();for(int i=0;i<3;++i)typing.backspace();touch(100,1364);touch(131,1132);
    if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){typing.draw();canvas.write_pgm((std::filesystem::path(folder)/"keyboard-symbols.pgm").string());}
    commit();assert(typing_store.feeds()[0].title=="@");
    edit_title();typing.text_input(std::string(3000,'a'));typing.submit();assert(typing_store.feeds()[0].title=="@");typing.back();typing.submit();assert(typing_store.feeds()[0].title=="@");
    {
        rss::Store names(":memory:");names.set_setting("reader_engine","classic");rss::App optional(names,canvas);
        auto tap=[&](int x,int y){optional.draw();optional.tap(x,y);};
        auto shot=[&](const char* name){optional.draw();if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(folder)/name).string());};
        tap(100,1392);tap(140,966);tap(950,140); // Add a feed without entering a name.
        tap(200,500);for(int i=0;i<8;++i)optional.backspace();optional.text_input("https://optional.test/rss");shot("keyboard-url.pgm");optional.submit();
        shot("optional-name-draft.pgm");tap(950,1380);assert(names.feeds().size()==1&&names.feeds()[0].title.empty());
        const auto id=names.feeds()[0].id;rss::ParsedFeed discovered;discovered.title="博客名称";names.save_entries(id,discovered);optional.reload();
        tap(950,398);tap(200,330);for(int i=0;i<4;++i)optional.backspace();optional.submit();
        assert(names.feeds()[0].title=="博客名称");tap(150,1380);assert(names.feeds()[0].title=="博客名称"); // Discard the empty draft.
        tap(950,398);tap(200,330);for(int i=0;i<4;++i)optional.backspace();optional.text_input("  ");optional.submit();
        optional.sync_started();bool rejected=false;try{tap(950,1380);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&names.feeds()[0].title=="博客名称");optional.sync_finished(true);tap(950,1380);assert(names.feeds()[0].title.empty());
        discovered.title="重新获取名称";names.save_entries(id,discovered);optional.reload();assert(names.feeds()[0].title=="重新获取名称");
        tap(950,398);tap(200,500);for(int i=0;i<25;++i)optional.backspace();rejected=false;try{optional.submit();}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&names.feeds()[0].url=="https://optional.test/rss");tap(62,60);tap(150,1380); // Cancel input and source edit.
        assert(names.feeds()[0].title=="重新获取名称");
        tap(950,398);tap(200,330);optional.text_input(std::string(700,'a'));shot("keyboard-long-input.pgm");tap(62,60);tap(150,1380);
        assert(names.feeds()[0].title=="重新获取名称");
    }

}
