#include "app.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Canvas canvas(1072,1448,font);rss::Store store(":memory:");store.set_setting("reader_engine","classic");
    const std::vector<std::string> names={"科技手记","开源观察","城市慢读","自然来信","设计日常","书页之间"};
    const std::vector<std::string> groups={"科技/数字生活","科技/开源与工具","人文与城市","自然与科学","科技/数字生活","人文与城市"};
    std::vector<std::int64_t> feeds;
    for(std::size_t i=0;i<names.size();++i){rss::Feed f;f.url="https://example.test/"+std::to_string(i);f.title=names[i];f.group=groups[i];feeds.push_back(store.add_feed(f));}
    const std::vector<int> source={0,1,2,3,0,1,5,4,3};std::vector<std::int64_t> entries;
    for(std::size_t i=0;i<source.size();++i){rss::Entry e;e.guid=std::to_string(i);e.title=i<2?"让旧设备继续工作：关于离线阅读与长期使用的实践记录":"值得慢慢阅读的文章 "+std::to_string(i);e.text="离线正文。\n第二段正文。";e.published=1700000000-static_cast<std::int64_t>(i);rss::ParsedFeed p;p.entries={e};store.save_entries(feeds[source[i]],p);entries.push_back(store.entries(rss::View::All,feeds[source[i]],e.title).front().id);}
    for(std::size_t i=0;i<entries.size();++i){store.set_read(entries[i],i>=7);store.set_starred(entries[i],i==3||i==6);}
    rss::App app(store,canvas);app.set_reader_status({68,false,true,"09:19"});
    const auto touch=[&](int x,int y){app.draw();app.tap(x,y);};
    const auto shot=[&](const char* name){app.draw();if(auto dir=std::getenv("RSS_TEST_SNAPSHOTS")){std::filesystem::create_directories(dir);canvas.write_pgm((std::filesystem::path(dir)/(std::string("navigation-")+name+".pgm")).string());}};
    const auto picker_row=[&](const std::string& group,bool all){auto g=store.groups();auto it=std::find(g.begin(),g.end(),group);assert(it!=g.end());return (!app.query().empty()?260:172)+88+static_cast<int>((it-g.begin()+(all?1:0))%6)*88+44;};
    const auto subscriptions=[&]{touch(100,1392);assert(app.menu_open());touch(140,966);assert(app.screen()==rss::App::Screen::Subscriptions);};
    shot("home");
    // The compact menu remains reachable without adding a second home title.
    touch(100,1392);assert(app.menu_open());touch(100,1392);assert(!app.menu_open());
    touch(200,38);assert(!app.menu_open()&&!app.reading());
    std::vector<std::int64_t> visible;
    for(int y=298;y<1335;y+=40){touch(100,y);if(app.reading()){auto id=app.current_id();if(visible.empty()||visible.back()!=id)visible.push_back(id);app.back();}}
    assert(visible.size()==7);
    for(std::size_t i=0;i<entries.size();++i)store.set_read(entries[i],i>=7);app.reload();
    // The dropdown preserves the current list page and consumes all background taps.
    app.show_view(rss::View::All);app.next();app.draw();const auto paper=canvas.pixels;
    touch(150,160);shot("group-dropdown-page1");assert(app.screen()==rss::App::Screen::GroupPicker);
    for(int y=0;y<1448;++y)for(int x=0;x<1072;++x)if(x<32||x>=1040||y<172||y>=880)assert(canvas.pixels[y*1072+x]==paper[y*1072+x]);
    app.next();shot("group-dropdown-page2");touch(940,852);assert(app.screen()==rss::App::Screen::GroupPicker);touch(100,776);assert(app.screen()==rss::App::Screen::GroupPicker&&!app.reading());
    touch(120,852);shot("group-dropdown-return");touch(1060,400);assert(app.screen()==rss::App::Screen::List&&!app.reading());app.draw();assert(canvas.pixels==paper);
    touch(150,160);touch(960,50);assert(app.screen()==rss::App::Screen::List&&app.take_request().empty());app.draw();assert(canvas.pixels==paper);
    app.show_view(rss::View::Unread);touch(150,160);touch(940,236);app.text_input("不存在的分组");app.submit();shot("group-dropdown-no-match");touch(200,346);assert(app.screen()==rss::App::Screen::GroupPicker);touch(940,530);assert(app.screen()==rss::App::Screen::List&&!app.reading());touch(150,160);touch(940,236);app.text_input("不存在的分组");app.submit();touch(940,236);shot("group-dropdown-cleared");app.back();assert(app.screen()==rss::App::Screen::List&&app.selected_group()==std::nullopt);assert(store.entries(rss::View::Unread).size()==7);
    // A menu dismisses outside taps, including taps over articles and sync.
    touch(100,1392);shot("brand-menu");touch(150,870);assert(app.menu_open());touch(150,700);assert(!app.menu_open()&&!app.reading());
    touch(100,1392);touch(960,50);assert(!app.menu_open()&&app.take_request().empty());
    // Home grouping is independent of subscription management.
    touch(150,160);assert(app.screen()==rss::App::Screen::GroupPicker);shot("group-picker");
    touch(940,236);app.text_input("科技");app.submit();shot("group-find");touch(200,236);app.text_input("取消");app.back();assert(app.screen()==rss::App::Screen::GroupPicker);touch(200,324);
    assert(app.selected_group()==std::optional<std::string>("科技"));shot("group-articles");
    touch(990,160);app.text_input("科技");app.submit();assert(app.query()=="科技");shot("search-results");
    touch(150,160);shot("group-dropdown-over-search");touch(940,324);app.text_input("人文");app.submit();app.back();assert(app.query()=="科技"&&app.selected_group()==std::optional<std::string>("科技"));
    touch(150,160);touch(200,412);assert(app.query()=="科技"&&app.selected_group()==std::nullopt);touch(150,160);touch(200,676);assert(app.query()=="科技"&&app.selected_group()==std::optional<std::string>("科技"));
    touch(930,226);assert(app.query().empty()&&app.selected_group()==std::optional<std::string>("科技"));
    touch(990,160);app.text_input("测试");app.submit();touch(990,160);app.backspace();app.backspace();app.submit();assert(app.query().empty());
    // A nested group search keeps a pending article keyword and composition.
    touch(990,160);app.text_input("初稿");touch(838,1132);touch(283,1016); // k, e
    touch(400,200);assert(app.screen()==rss::App::Screen::GroupPicker);touch(950,140);app.text_input("科技");app.submit();app.back();
    app.submit();assert(app.query().empty());app.back();app.text_input("词");app.submit();assert(app.query()=="初稿词");
    touch(930,226);assert(app.query().empty());
    touch(150,160);touch(200,picker_row("",true));shot("empty-group");touch(100,400);assert(!app.reading());
    // Background completion changes task state without replacing the cache page.
    touch(100,1392);touch(140,1174);assert(app.take_request()=="sync"&&!app.menu_open());shot("sync-running");touch(800,904);assert(app.screen()==rss::App::Screen::List&&app.sync_state()==rss::App::SyncState::Running&&app.take_request().empty());touch(100,1392);touch(140,1174);assert(app.screen()==rss::App::Screen::Sync&&app.take_request().empty());touch(880,548);assert(app.screen()==rss::App::Screen::Cache);
    app.sync_progress(6,6,0,"更新完成");app.sync_finished(false);assert(app.screen()==rss::App::Screen::Cache);shot("cache");app.back();assert(app.screen()==rss::App::Screen::Sync&&app.sync_state()==rss::App::SyncState::Complete);shot("sync-complete");app.back();
    touch(100,1392);touch(140,1174);assert(app.take_request()=="sync");store.sync_result(feeds[1],"连接超时","","",0);app.sync_progress(6,6,1,"有订阅失败");app.sync_finished(false);assert(app.sync_state()==rss::App::SyncState::Failed);
    touch(200,440);shot("edit-failed");touch(100,50);assert(app.screen()==rss::App::Screen::Sync);
    touch(200,440);touch(200,330);for(int i=0;i<4;++i)app.backspace();app.text_input("开源修订");app.submit();assert(store.feeds()[1].title=="开源观察");touch(950,1380);assert(store.feeds()[1].title=="开源修订"&&app.screen()==rss::App::Screen::Sync);shot("retry-after-edit");
    touch(200,1124);assert(app.take_request()=="retry-sync");store.sync_result(feeds[1],"","","",1700000000);app.sync_progress(1,1,0,"更新完成");app.sync_finished(false);app.back();
    // Group metadata survives empty folders, rename, cancel and delete.
    subscriptions();shot("subscriptions");touch(150,1380);assert(app.screen()==rss::App::Screen::Groups);touch(950,140);touch(200,350);app.text_input("测试");app.submit();touch(950,1380);
    auto all=store.groups();assert(std::find(all.begin(),all.end(),"测试")!=all.end());shot("groups");
    auto group_edit=[&](const std::string& path){auto g=store.groups();auto it=std::find(g.begin(),g.end(),path);assert(it!=g.end());touch(950,250+static_cast<int>(it-g.begin())*128+44);assert(app.screen()==rss::App::Screen::GroupEdit);};
    group_edit("测试");touch(200,350);app.backspace();app.backspace();app.text_input("阅读");app.submit();touch(950,1380);all=store.groups();assert(std::find(all.begin(),all.end(),"阅读")!=all.end());
    group_edit("阅读");touch(200,820);shot("delete-group");touch(200,1380);all=store.groups();assert(std::find(all.begin(),all.end(),"阅读")!=all.end());touch(200,820);touch(950,1380);all=store.groups();assert(std::find(all.begin(),all.end(),"阅读")==all.end());assert(store.feeds().size()==6&&store.entries(rss::View::All).size()==9);
    app.back();touch(950,1380);assert(app.screen()==rss::App::Screen::Opml);shot("opml");touch(200,350);touch(200,1380);assert(app.screen()==rss::App::Screen::Opml);touch(200,550);assert(app.take_request()=="export");touch(200,350);touch(950,1380);assert(app.take_request()=="import");app.back();app.back();
    // Every failed source stays reachable, and returning home can start a full sync.
    for(auto id:feeds)store.sync_result(id,"同步失败","","",0);
    touch(100,1392);touch(140,1174);assert(app.take_request()=="sync");app.sync_finished(false);shot("sync-failures");app.next();shot("sync-failures-page2");
    touch(200,440);touch(200,330);for(int i=0;i<4;++i)app.backspace();app.text_input("末页源");app.submit();touch(950,1380);
    assert(store.feeds()[4].title=="末页源");app.back();touch(100,1392);touch(140,1174);assert(app.take_request()=="sync");
    // A changed address is only a draft, and cannot replace an active download's source.
    app.sync_finished(false);touch(200,440);touch(200,500);for(int i=0;i<22;++i)app.backspace();app.text_input("https://changed.test/rss");app.submit();
    app.sync_started();bool blocked=false;try{touch(950,1380);}catch(const std::runtime_error&){blocked=true;}assert(blocked&&store.feeds()[0].url=="https://example.test/0");
    app.back();assert(store.feeds()[0].url=="https://example.test/0");app.sync_finished(true);app.back();
    // Exit is a separate confirmation; cancellation never quits or clicks through.
    touch(100,1392);touch(140,1278);shot("exit-confirm");touch(200,844);assert(!app.quit);
    touch(100,1392);touch(140,1278);touch(100,900);assert(!app.quit);
    touch(100,1392);touch(140,1278);touch(500,680);touch(850,844);assert(app.quit);
    {
        rss::Store empty_store(":memory:");empty_store.set_setting("reader_engine","classic");rss::App empty_app(empty_store,canvas);
        auto capture=[&](const char* name){empty_app.draw();if(auto dir=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(dir)/(std::string("empty-")+name+".pgm")).string());};
        auto action=[&]{
            empty_app.draw();
            for(int y=550;y+88<1344;++y){bool border=true;for(int row=0;row<88;++row)if(canvas.pixels[(y+row)*1072+32]>100){border=false;break;}
                if(border){empty_app.tap(120,y+44);return;}}
            assert(false&&"Visible empty-state button must fit above the footer");
        };
        capture("no-subscriptions");action();assert(empty_app.screen()==rss::App::Screen::Subscriptions&&empty_store.feeds().empty());empty_app.back();empty_app.back();
        rss::Feed f;f.url="https://empty.test/rss";f.title="空态测试";f.group="测试";auto fid=empty_store.add_feed(f);empty_app.show_group("");capture("ungrouped");action();assert(empty_app.screen()==rss::App::Screen::Subscriptions);empty_app.back();assert(empty_app.selected_group()==std::optional<std::string>(""));
        rss::Entry e;e.guid="read";e.title="已经读完";e.text="正文";rss::ParsedFeed parsed;parsed.entries={e};empty_store.save_entries(fid,parsed);auto eid=empty_store.entries(rss::View::All).front().id;empty_store.set_read(eid,true);
        empty_app.show_view(rss::View::Unread);capture("done");action();empty_app.draw();empty_app.tap(100,300);assert(empty_app.current_id()==eid);empty_app.back();
        empty_app.show_view(rss::View::Starred);capture("starred");action();assert(empty_app.screen()==rss::App::Screen::Subscriptions);empty_app.back();
        empty_app.show_view(rss::View::All);empty_app.draw();empty_app.tap(990,160);empty_app.text_input("没有这个标题");empty_app.submit();capture("search");action();empty_app.text_input("修订");empty_app.submit();assert(empty_app.query()=="没有这个标题修订");
        empty_app.show_view(rss::View::All);empty_app.draw();empty_app.tap(990,160);std::string long_query;for(int i=0;i<300;++i)long_query+="长查询";empty_app.text_input(long_query.substr(0,1800));empty_app.submit();capture("long-search");action();empty_app.back();assert(!empty_app.query().empty()&&empty_store.feeds().size()==1&&empty_store.entry(eid)->read);
    }

    {
        // Manage a complete tree through the same touch targets used on Kindle.
        rss::Store tree(":memory:");tree.set_setting("reader_engine","classic");tree.add_group("Alpha/Existing/Deep");tree.add_group("Beta");
        rss::App nested(tree,canvas);
        auto tap=[&](int x,int y){nested.draw();nested.tap(x,y);};
        auto capture=[&](const char* name){nested.draw();if(auto dir=std::getenv("RSS_TEST_SNAPSHOTS"))canvas.write_pgm((std::filesystem::path(dir)/(std::string("tree-")+name+".pgm")).string());};
        auto has=[&](const std::string& path){auto g=tree.groups();return std::find(g.begin(),g.end(),path)!=g.end();};
        auto manage=[&]{tap(100,1392);tap(140,966);tap(150,1380);assert(nested.screen()==rss::App::Screen::Groups);};
        auto row=[&](const std::string& path,int x,int page=0,int item=-1){
            auto g=tree.groups();auto it=std::find(g.begin(),g.end(),path);assert(it!=g.end());
            const auto index=static_cast<std::size_t>(it-g.begin());
            for(std::size_t i=0;i<g.size();++i)nested.previous();
            for(int i=0;i<page;++i){nested.draw();nested.next();}
            tap(x,250+(item<0?static_cast<int>(index):item)*128+44);assert(nested.screen()==rss::App::Screen::GroupEdit);
        };
        auto name=[&](const std::string& value){tap(200,350);for(int i=0;i<100;++i)nested.backspace();nested.text_input(value);nested.submit();};
        auto parent=[&](const std::string& query){
            tap(200,550);assert(nested.screen()==rss::App::Screen::GroupPicker);tap(950,140);nested.text_input(query);nested.submit();
            tap(200,396);assert(nested.screen()==rss::App::Screen::GroupEdit);
        };
        auto save=[&]{tap(950,1380);assert(nested.screen()==rss::App::Screen::Groups);};
        manage();row("Alpha",800);name("Child");capture("new-child");assert(!has("Alpha/Child"));save();assert(has("Alpha/Child"));
        row("Alpha/Child",800);name("Nested");save();assert(has("Alpha/Child/Nested"));
        tap(950,140);name("FromPicker");parent("Alpha/Child");save();assert(has("Alpha/Child/FromPicker"));
        // A changed parent remains a draft; backing out of its picker preserves the name.
        row("Alpha/Child",950);name("Draft");parent("Beta");nested.draw();const auto draft_form=canvas.pixels;tap(200,550);nested.back();nested.draw();assert(canvas.pixels==draft_form);tap(200,1380);
        assert(has("Alpha/Child")&&!has("Beta/Draft"));row("Alpha/Child",950);save();assert(has("Alpha/Child"));
        // Self and descendants are absent even when they match a parent search.
        row("Alpha/Child",950);tap(200,550);tap(950,140);nested.text_input("Alpha/Child");nested.submit();capture("parent-no-cycle");
        tap(200,396);assert(nested.screen()==rss::App::Screen::GroupPicker);nested.back();nested.back();
        // Follow an active descendant filter when moving an entire subtree.
        nested.back();nested.back();nested.show_group("Alpha/Child/Nested");manage();row("Alpha/Child",950);parent("Beta");capture("move-draft");
        assert(has("Alpha/Child/Nested")&&!has("Beta/Child"));save();assert(!has("Alpha/Child")&&has("Beta/Child/Nested")&&has("Beta/Child/FromPicker"));
        nested.back();nested.back();assert(nested.selected_group()==std::optional<std::string>("Beta/Child/Nested"));
        manage();row("Beta/Child",950);parent("顶层");save();assert(has("Child/Nested")&&has("Beta")&&!has("Beta/Child"));
        // Destination collision fails atomically and keeps the form recoverable.
        tree.add_group("Alpha/Child");nested.reload();row("Child",950,1,0);parent("Alpha");
        bool rejected=false;try{save();}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&nested.screen()==rss::App::Screen::GroupEdit&&has("Child/Nested")&&has("Alpha/Child"));nested.back();
        // Paginated parent lists can be searched and cleared without losing the draft.
        for(int i=0;i<14;++i)tree.add_group("Z"+std::to_string(i));nested.reload();tap(950,140);name("LastChild");tap(200,550);nested.next();capture("parent-page2");
        tap(950,140);nested.text_input("Z9");nested.submit();capture("parent-search");tap(200,396);save();assert(has("Z9/LastChild"));capture("groups-page");
        // Delete still includes descendants and preserves other trees.
        row("Child",950,1,0);tap(200,820);tap(950,1380);assert(!has("Child/Nested")&&has("Alpha/Existing/Deep")&&has("Z9/LastChild"));
    }
    std::cout<<"navigation checks passed\n";
}
