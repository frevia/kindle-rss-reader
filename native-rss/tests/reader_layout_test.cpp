#include "app.hpp"
#include "cre_reader.hpp"
#include "feed.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Canvas canvas(1072,1448,font);
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://status-font.test/rss";feed.title="状态字体";
        const auto id=store.add_feed(feed);rss::ParsedFeed batch;
        rss::Entry entry;entry.guid="status-font";entry.title="独立状态栏字体";entry.text="正文和返回按钮应保持不变。";
        batch.entries.push_back(entry);store.save_entries(id,batch);
        const auto article=store.entries(rss::View::All).front().id;
        rss::App fallback(store,canvas);fallback.set_reader_status({69,false,true,"22:05"});fallback.draw();
        const auto list=canvas.pixels;
        rss::Canvas expected_clock(1072,1448,font);expected_clock.clear();
        expected_clock.text(32,48,"22:05",31,0);
        for(int y=0;y<64;++y)for(int x=0;x<672;++x)assert(list[y*1072+x]==expected_clock.pixels[y*1072+x]);
        fallback.open(article,false);fallback.draw();const auto reader=canvas.pixels;
        for(int y=0;y<76;++y)for(int x=700;x<1072;++x)assert(reader[y*1072+x]==list[y*1072+x]);
        expected_clock.clear();expected_clock.text(32,1448-20,"22:05",31,80);
        for(int y=1380;y<1448;++y)for(int x=0;x<240;++x)assert(reader[y*1072+x]==expected_clock.pixels[y*1072+x]);
        for(const auto& status_font:{font,std::string("/missing/rss-status-font.ttf")}){
            rss::App app(store,canvas,0,status_font);app.set_reader_status({69,false,true,"22:05"});app.draw();
            assert(canvas.pixels==list);
            app.open(article,false);app.draw();assert(canvas.pixels==reader);
        }
    }
    {
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://ui.test/rss";feed.title="界面一致性";store.add_feed(feed);store.add_group("设计");
        rss::App app(store,canvas);app.set_reader_status({68,false,true,"09:19"});
        const auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
        app.draw();const auto home_status=std::vector<std::uint8_t>(canvas.pixels.begin(),canvas.pixels.begin()+64*1072);
        const auto capture=[&](const char* name){
            app.draw();
            for(int y=0;y<64;++y)for(int x=700;x<1072;++x)assert(canvas.pixels[y*1072+x]==home_status[y*1072+x]);
            if(auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){
                std::filesystem::create_directories(folder);
                canvas.write_pgm((std::filesystem::path(folder)/(std::string("ui-")+name+".pgm")).string());
            }
        };
        tap(100,1392);tap(140,966);capture("subscriptions");const auto header=canvas.pixels;
        rss::Canvas centered_clock(1072,1448,font);
        centered_clock.text((1072-centered_clock.measure("09:19",31))/2,48,"09:19",31,0);
        for(int y=0;y<64;++y)for(int x=400;x<650;++x)assert(header[y*1072+x]==centered_clock.pixels[y*1072+x]);
        const auto same_back=[&](const char* name){
            capture(name);
            for(int y=6;y<70;++y)for(int x=24;x<120;++x)assert(canvas.pixels[y*1072+x]==header[y*1072+x]);
            for(int y=0;y<70;++y)for(int x=400;x<1072;++x)assert(canvas.pixels[y*1072+x]==header[y*1072+x]);
        };
        tap(950,140);same_back("add-feed");tap(200,330);app.text_input("尚未保存");same_back("name-input");
        tap(62,60);tap(62,60);assert(store.feeds().front().title=="界面一致性");
        tap(150,1380);same_back("groups");tap(950,140);same_back("group-edit");
        tap(200,550);same_back("parent-picker");tap(62,60);tap(62,60);tap(62,60);
        assert(app.screen()==rss::App::Screen::Subscriptions&&store.groups().size()==2);
        tap(950,1380);same_back("opml");tap(200,350);same_back("confirmation");tap(62,60);
        assert(app.screen()==rss::App::Screen::Opml&&app.take_request().empty());tap(62,60);tap(62,60);
        tap(990,160);app.text_input("未确认的关键词");same_back("search");tap(400,246);same_back("search-scope");
        tap(950,140);app.text_input("设计");same_back("find-group");tap(62,60);tap(62,60);
        app.submit();assert(app.query()=="未确认的关键词");app.show_view(rss::View::All);
        tap(100,1392);tap(140,1174);assert(app.take_request()=="sync");capture("sync");tap(880,548);same_back("cache");
        tap(200,1380);same_back("clean-confirm");tap(62,60);tap(62,60);app.sync_finished(true);tap(800,904);
        app.show_error("界面检查示例：操作暂时无法完成");same_back("error");tap(62,60);
        assert(app.screen()==rss::App::Screen::List&&store.feeds().size()==1&&store.groups().size()==2);
    }
    std::vector<std::string> engines{"classic"};
    if(rss::CreReader::available())engines.push_back("cre");
    for(const auto& engine:engines){
        rss::Store store(":memory:");store.set_setting("reader_engine",engine);
        rss::Feed feed;feed.url="https://layout-"+engine+".test/rss";feed.title="布局回归";
        const auto feed_id=store.add_feed(feed);rss::ParsedFeed batch;
        for(int i=0;i<2;++i){
            rss::Entry article;article.guid=engine+"-"+std::to_string(i);article.title="布局文章 "+std::to_string(i);
            article.url="https://layout-"+engine+".test/article-"+std::to_string(i);
            article.text="文章标记 "+std::to_string(i)+"。";
            for(int n=0;n<180;++n)article.text+="唯一段落 "+std::to_string(n)+"：用于验证工具栏、状态栏和设置面板不会改变文章的阅读分页与锚点位置。 ";
            batch.entries.push_back(std::move(article));
        }
        store.save_entries(feed_id,batch);const auto entries=store.entries(rss::View::All);
        assert(entries.size()==2);rss::App app(store,canvas);
        const auto capture=[&](const char* name){
            if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){
                std::filesystem::create_directories(folder);
                canvas.write_pgm((std::filesystem::path(folder)/(engine+"-"+name+".pgm")).string());
            }
        };
        const auto aligned_status=[&](int top){
            for(const auto level:{9,14,100,-1}){
                rss::ReaderStatus sample{level,level==100,false,"09:19"};
                app.set_reader_status(sample);app.draw();const auto without_wifi=canvas.pixels;
                sample.wifi=true;app.set_reader_status(sample);app.draw();
                int left=1072,right=0,first_y=1448,last_y=0;
                for(int y=top;y<top+34;++y)for(int x=650;x<1040;++x)
                    if(canvas.pixels[y*1072+x]!=without_wifi[y*1072+x]){left=std::min(left,x);right=std::max(right,x);first_y=std::min(first_y,y);last_y=std::max(last_y,y);}
                assert(left<right);
                const auto ink=[&](int x){for(int y=top;y<top+34;++y)if(without_wifi[y*1072+x]<180)return true;return false;};
                int battery_left=650;while(battery_left<1040&&!ink(battery_left))++battery_left;
                assert(battery_left-right>=6&&battery_left-right<=20);
                int battery_right=battery_left;while(battery_right<1040&&ink(battery_right))++battery_right;
                assert(battery_right-battery_left==41);
                int text_left=battery_right;while(text_left<1040&&!ink(text_left))++text_left;
                assert(text_left-battery_right>=6&&text_left-battery_right<=20);
                int battery_top=1448,battery_bottom=0;
                for(int y=top;y<top+34;++y)for(int x=battery_left;x<battery_right;++x)
                    if(without_wifi[y*1072+x]<180){battery_top=std::min(battery_top,y);battery_bottom=std::max(battery_bottom,y);}
                assert(std::abs(first_y+last_y-battery_top-battery_bottom)<=6);
            }
        };
        aligned_status(17);
        app.draw();const auto list_before=canvas.pixels;
        capture("list");
        app.dirty=false;app.set_reader_status({68,false,true,"09:19"});assert(app.dirty);app.draw();
        assert(std::equal(list_before.begin()+112*1072,list_before.end(),canvas.pixels.begin()+112*1072));
        app.dirty=false;app.set_reader_status({68,false,true,"09:19"});assert(!app.dirty);
        app.tap(100,1392);assert(app.menu_open());app.draw();app.tap(120,966);app.draw();
        assert(app.screen()==rss::App::Screen::Subscriptions);const auto navigation=canvas.pixels;
        app.set_reader_status({67,false,true,"09:20"});assert(app.dirty);app.draw();
        assert(std::equal(navigation.begin()+112*1072,navigation.end(),canvas.pixels.begin()+112*1072));
        app.back();app.draw();
        app.tap(100,1392);app.draw();app.tap(140,1174);assert(app.take_request()=="sync");app.back();

        app.set_reader_status({68,false,true,"09:19"});app.draw();
        const auto status_strip=std::vector<std::uint8_t>(canvas.pixels.begin(),canvas.pixels.begin()+64*1072);
        const auto same_status=[&]{for(int y=0;y<64;++y)for(int x=700;x<1072;++x)assert(status_strip[y*1072+x]==canvas.pixels[y*1072+x]);};
        app.open(entries.front().id);app.draw();assert(app.reading());same_status();
        aligned_status(17);
        capture("reader");
        assert(app.reader_page()==0);app.next();app.next();app.draw();assert(app.reader_page()==2);
        const auto saved_anchor=store.setting("reader_cre:"+std::to_string(app.current_id()));
        const auto normal=canvas.pixels;
        for(int y=0;y<96;++y)for(int x=250;x<800;++x)assert(normal[y*1072+x]==255);
        bool body_ink=false;for(int y=120;y<700;++y)for(int x=32;x<1040;++x)if(normal[y*1072+x]<220)body_ink=true;
        assert(body_ink);
        app.dirty=false;app.set_reader_status({67,true,false,"09:20"});assert(app.dirty);app.draw();
        assert(std::equal(normal.begin()+96*1072,normal.begin()+700*1072,canvas.pixels.begin()+96*1072));
        const auto status_page=app.reader_page();const auto status_position=store.entry(app.current_id())->position;
        app.set_reader_status({68,false,true,"09:19"});app.draw();
        assert(app.reader_page()==status_page&&store.entry(app.current_id())->position==status_position);

        app.tap(536,600);app.draw();assert(app.reader_tools_open());
        assert(store.setting("reader_cre:"+std::to_string(app.current_id()))==saved_anchor);
        const auto tool_page=app.reader_page();const auto tool_position=store.entry(app.current_id())->position;
        const auto article_before=canvas.pixels;
        app.tap(178,1340);app.draw();assert(app.reader_panel()==rss::App::ReaderPanel::None);
        assert(app.reader_page()==tool_page&&store.entry(app.current_id())->position==tool_position);
        if(!app.reader_tools_open()){app.tap(536,600);app.draw();assert(app.reader_tools_open());}
        app.tap(178,1400);app.draw();assert(app.reader_panel()==rss::App::ReaderPanel::Type);
        assert(app.reader_page()==tool_page&&store.entry(app.current_id())->position==tool_position);
        // Classic layout omits the entire 58px line box when it intersects the panel at y712.
        // Compare only the fully visible area; page and anchor checks above cover repagination.
        constexpr int unobscured_bottom=712-58;
        assert(std::equal(article_before.begin()+96*1072,article_before.begin()+unobscured_bottom*1072,canvas.pixels.begin()+96*1072));
        capture("type-panel");
        app.tap(62,54);app.draw();assert(app.reading()&&app.reader_tools_open()&&app.reader_panel()==rss::App::ReaderPanel::None);
        app.tap(62,54);app.draw();assert(app.reading()&&!app.reader_tools_open());
        assert(store.setting("reader_cre:"+std::to_string(app.current_id()))==saved_anchor);

        app.tap(536,600);app.draw();assert(app.reader_tools_open());
        app.tap(410,1400);app.draw();assert(app.reader_panel()==rss::App::ReaderPanel::Content);
        capture("content-panel");
        const auto content_page=app.reader_page();const auto content_position=store.entry(app.current_id())->position;
        assert(store.queue_fulltext(app.current_id()));rss::Entry full;full.url=entries.front().url;full.html="<p>网页正文回归夹具</p>";full.text="网页正文回归夹具";store.save_fulltext(app.current_id(),full);assert(store.fulltext_status(app.current_id()).state=="ready");
        app.draw();
        app.tap(800,1002);app.draw();assert(app.using_fulltext());
        app.tap(280,1002);app.draw();assert(!app.using_fulltext());
        assert(app.reader_page()==content_page&&store.entry(app.current_id())->position==content_position);
        app.tap(62,54);app.draw();assert(app.reading()&&app.reader_tools_open());
        app.tap(178,1400);app.draw();app.tap(960,856);app.draw();
        assert(app.reader_panel()==rss::App::ReaderPanel::Type&&store.setting("font_size")=="36");
        assert(store.entry(app.current_id())->position==content_position);
        app.tap(62,54);app.draw();assert(app.reading()&&app.reader_tools_open());
        app.tap(62,54);assert(app.reading()&&!app.reader_tools_open());
        app.tap(62,54);assert(!app.reading());
    }
    std::cout<<"reader layout checks passed\n";
}
