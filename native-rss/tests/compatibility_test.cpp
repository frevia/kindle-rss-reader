#include "app.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main(){
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path();
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):(root/"vendor/runtime/reader.ttf").string();
    if(!std::filesystem::exists(font))font="/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    for(const auto size:{rss::TouchPoint{600,800},{758,1024},{1072,1448},{1236,1648},
                         {1264,1680},{1404,1872},{1860,2480}}){
        rss::ScreenTransform screen(size.x,size.y);
        rss::Store store(":memory:");store.set_setting("reader_engine","classic");
        rss::Feed feed;feed.url="https://compatibility.invalid/rss";feed.title="机型适配测试";
        const auto fid=store.add_feed(feed);
        rss::Entry entry;entry.guid="article";entry.title="不同尺寸的阅读与输入";
        for(int i=0;i<100;++i)entry.text+="屏幕尺寸变化后，文字、按钮与触摸位置应保持一致。\n\n";
        rss::ParsedFeed batch;batch.entries={entry};store.save_entries(fid,batch);
        rss::Canvas canvas(1072,1448,font);rss::App app(store,canvas);
        auto tap=[&](int x,int y){
            const rss::TouchPoint physical{screen.x+x*(screen.width-1)/1071,screen.y+y*(screen.height-1)/1447};
            const auto logical=screen.to_logical(physical);assert(logical);
            assert(std::abs(logical->x-x)<=2&&std::abs(logical->y-y)<=2);
            app.draw();app.tap(logical->x,logical->y);
        };
        auto snapshot=[&](const char* name){
            app.draw();const auto pixels=screen.scale(canvas.pixels);
            assert(pixels.size()==static_cast<std::size_t>(size.x)*size.y);
            for(int y=0;y<size.y;++y)for(int x=0;x<size.x;++x)
                if(x<screen.x||x>=screen.x+screen.width||y<screen.y||y>=screen.y+screen.height)
                    assert(pixels[static_cast<std::size_t>(y)*size.x+x]==255);
            if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){
                std::filesystem::create_directories(folder);
                const auto path=std::filesystem::path(folder)/(std::to_string(size.x)+"x"+std::to_string(size.y)+"-"+name+".pgm");
                std::ofstream out(path,std::ios::binary);out<<"P5\n"<<size.x<<" "<<size.y<<"\n255\n";
                out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());assert(out.good());
            }
        };
        snapshot("list");tap(150,244);assert(app.reading());snapshot("reader");
        const auto page=app.reader_page();tap(950,1380);assert(app.reader_page()==page+1);
        tap(48,48);assert(!app.reading());
        tap(100,1392);assert(app.menu_open());tap(140,966);
        assert(app.screen()==rss::App::Screen::Subscriptions);
        tap(950,140);tap(200,500);for(int i=0;i<8;++i)app.backspace();
        app.text_input("https://new.invalid/rss");snapshot("keyboard");const auto keyboard=canvas.pixels;
        tap(950,1364);snapshot("draft");assert(canvas.pixels!=keyboard);
        assert(store.feeds().size()==1&&store.feeds().front().url==feed.url);
    }
}
