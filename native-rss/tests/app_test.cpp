#include "app.hpp"
#include "feed.hpp"
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Store store(":memory:");rss::Canvas canvas(1072,1448,font);
    {
        rss::Store pages_store(":memory:");rss::Feed feed;feed.url="https://pages.example/feed";
        const auto feed_id=pages_store.add_feed(feed);rss::ParsedFeed batch;
        for(int i=0;i<45;++i){rss::Entry item;item.guid=std::to_string(i);
            item.title=i%3==0?std::string(110,'x'):"文章 "+std::to_string(i);item.text="正文";batch.entries.push_back(item);}
        pages_store.save_entries(feed_id,batch);const auto ordered=pages_store.entries(rss::View::All,feed_id);
        rss::App pages(pages_store,canvas);pages.show_view(rss::View::All,feed_id);
        std::vector<std::vector<std::pair<std::int64_t,int>>> expected(1);
        int y=280;
        for(const auto& item:ordered){
            const int height=static_cast<int>(std::min<std::size_t>(3,canvas.wrap(item.title,36,914).size()))*46+94;
            if(y+height>1268&&!expected.back().empty()){expected.emplace_back();y=280;}
            expected.back().push_back({item.id,y+40});y+=height;
        }
        auto first_id=[&]{pages.draw();pages.tap(100,320);const auto id=pages.current_id();pages.back();return id;};
        for(std::size_t p=0;p<expected.size();++p){
            for(const auto& row:expected[p]){
                pages.draw();pages.tap(100,row.second);assert(pages.current_id()==row.first);pages.back();
            }
            if(p+1<expected.size())pages.next();
        }
        pages.next();assert(first_id()==expected.back().front().first);
        for(auto p=expected.size()-1;p>0;--p){pages.previous();assert(first_id()==expected[p-1].front().first);}
        pages.previous();assert(first_id()==ordered.front().id);
        // Navigation may arrive before the next draw, and reload preserves the ordinal.
        pages.next();pages.next();assert(first_id()==expected[2].front().first);
        pages.reload();assert(first_id()==expected[2].front().first);
        // Shrinking the snapshot clamps to the remaining page, then to the empty page.
        pages_store.set_starred(ordered.back().id,true);pages_store.remove_feed(feed_id);
        pages.reload();assert(first_id()==ordered.back().id);
        pages.next();assert(first_id()==ordered.back().id);
        pages_store.set_starred(ordered.back().id,false);pages_store.remove_feed(feed_id);
        pages.reload();pages.next();pages.previous();pages.draw();pages.tap(100,320);assert(!pages.reading());
        assert(ordered.size()==45);
    }
    rss::Feed f;f.url="https://example.com/feed";f.title="测试来源";auto id=store.add_feed(f);
    rss::Entry e;e.guid="one";e.title="中文文章标题";
    for(int i=0;i<200;++i)e.text+="阅读位置必须跨字号保留，返回列表不应自动重排。\n";
    rss::ParsedFeed parsed;parsed.entries={e};store.save_entries(id,parsed);
    auto entry=store.entries(rss::View::All)[0];rss::App app(store,canvas);app.draw();app.tap(100,320);
    assert(app.reading());assert(store.entry(entry.id)->read);
    app.next();app.next();assert(app.reader_page()==2);const auto position=store.entry(entry.id)->position;assert(position>0);
    app.back();assert(!app.reading());app.draw();app.tap(100,320);assert(app.reading());assert(app.reader_page()==2);
    app.draw();app.tap(530,400);app.draw();app.tap(960,1050);app.draw(); // font + in reader settings
    assert(app.reading());app.back();app.back();app.show_view(rss::View::Unread);app.draw();
    assert(store.entries(rss::View::Unread).empty());
    app.show_view(rss::View::All);app.draw();app.tap(100,320);app.draw();app.tap(530,400);app.draw();
    app.tap(250,970);assert(store.entry(entry.id)->starred);
    app.back();app.show_view(rss::View::Starred);app.draw();
    assert(store.entries(rss::View::Starred).size()==1);
    // Management actions require explicit confirmation; cancellation changes no data.
    auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
    auto snapshot=[&](const char* name){
        if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){
            std::filesystem::create_directories(folder);app.draw();
            canvas.write_pgm((std::filesystem::path(folder)/name).string());
        }
    };
    tap(100,150);tap(950,440);snapshot("subscription.pgm");
    tap(400,920);snapshot("unsubscribe.pgm");tap(200,800); // unsubscribe then cancel
    assert(store.feeds().size()==1);
    tap(950,60);tap(950,60); // back to navigation, then list
    store.set_read(entry.id,false);
    tap(100,150);tap(950,440);tap(400,800);tap(800,800);
    assert(store.entry(entry.id)->read);
    tap(100,150);tap(950,440);tap(400,920);tap(800,800);
    assert(store.feeds().empty());assert(store.entry(entry.id)->starred);
    rss::Feed other;other.url="https://example.com/other";other.title="Other";
    const auto other_id=store.add_feed(other);
    rss::ParsedFeed old;rss::Entry clean;clean.guid="clean";clean.title="Old";clean.text="text";clean.published=100;
    old.entries={clean};store.save_entries(other_id,old);
    const auto clean_id=store.entries(rss::View::All,other_id)[0].id;
    store.set_read(clean_id,true);app.reload();
    tap(100,150);tap(950,360);tap(400,800);tap(200,800);
    assert(store.entry(clean_id)); // cancel cleanup
    tap(400,800);tap(800,800);
    assert(!store.entry(clean_id));assert(store.entry(entry.id));
    tap(100,150);tap(950,440);tap(200,1040);
    for(int i=0;i<5;++i)app.backspace();
    app.text_input("新的名称");app.submit();assert(store.feeds()[0].title=="新的名称");
    tap(800,1040);app.text_input("科技/编程");app.submit();
    assert(store.feeds()[0].group=="科技/编程");
    tap(200,1040);app.text_input("取消修改");app.back();
    assert(store.feeds()[0].title=="新的名称");
    tap(950,60);tap(270,360);snapshot("groups.pgm");tap(300,440);
    old.entries[0].guid="new-child";
    rss::ArticleImage photo;photo.url="https://example.test/photo.png";photo.alt="离线图片";
    rss::ArticleImage missing;missing.url="https://example.test/missing.png";
    old.entries[0].images={photo,missing};store.save_entries(other_id,old);app.reload();
    tap(100,320);assert(app.current_id()==store.entries(rss::View::All,other_id)[0].id);
    photo.width=320;photo.height=180;photo.pixels.resize(320*180);
    for(int y=0;y<180;++y)for(int x=0;x<320;++x)photo.pixels[y*320+x]=x%256;
    store.save_image(app.current_id(),photo);
    tap(900,50);app.draw();assert(canvas.pixels[695*1072+536]==160);
    snapshot("image.pgm");app.next();snapshot("image-missing.pgm");app.back();assert(app.reading());
    const auto current=app.current_id();
    app.show_error("网络暂时不可用，已有文章仍可阅读");
    tap(100,320);assert(app.current_id()==current);app.back();assert(app.reading());
    // An image-only article must reopen the exact image page despite identical text offsets.
    {
        rss::Store image_store(":memory:");rss::Feed feed;feed.url="https://example.test/rss";
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
        reader.draw();reader.tap(536,400);reader.draw();reader.back();assert(reader.reader_page()==2);
        reader.previous();assert(reader.reader_page()==1);reader.draw();reader.tap(536,400);reader.draw();
        assert(canvas.pixels[500*1072+536]==155);reader.back();assert(reader.reader_page()==1);
        reader.draw();reader.tap(950,1400);reader.draw();reader.tap(960,1050);reader.back();
        reader.back();reader.open(eid);assert(reader.reader_page()==1);
    }
    std::cout<<"app checks passed\n";
    // Touch-only Chinese input: compose, correct, select, page candidates, and commit a feed name.
    rss::Store typing_store(":memory:");rss::Feed typing_feed;typing_feed.url="https://example.test/rss";typing_feed.title="旧名";
    const auto typing_id=typing_store.add_feed(typing_feed);rss::App typing(typing_store,canvas);
    auto touch=[&](int x,int y){typing.draw();typing.tap(x,y);};
    auto keys=[&](const std::string& value){
        const std::vector<std::string> rows={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm","./:-_?=&%"};
        for(char ch:value){bool found=false;
            for(std::size_t r=0;r<rows.size();++r){const auto column=rows[r].find(ch);if(column!=std::string::npos){touch(80+static_cast<int>(column)*100,923+static_cast<int>(r)*76);found=true;break;}}
            assert(found);
        }
    };
    touch(100,150);touch(950,440);touch(200,1040);
    typing.backspace();typing.backspace();keys("kejix");typing.backspace();
    typing.submit();assert(typing_store.feeds()[0].title=="旧名");typing.back(); // pending composition cannot silently disappear
    if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")){typing.draw();canvas.write_pgm((std::filesystem::path(folder)/"pinyin.pgm").string());}
    touch(180,655);touch(850,1310);assert(typing_store.feeds()[0].title=="科技");
    touch(200,1040);typing.backspace();typing.backspace();keys("shi");touch(900,820);touch(180,655);
    touch(850,1310);assert(typing_store.feeds()[0].title!="科技");assert(!typing_store.feeds()[0].title.empty());
    touch(200,1040);typing.backspace();keys("ni");touch(850,1390); // switching to English retains the literal spelling
    keys("rss");touch(850,1310);assert(typing_store.feeds()[0].title=="nirss");
    assert(typing_store.feeds()[0].id==typing_id);
    touch(200,1040);typing.text_input(std::string(3000,'a'));typing.submit();
    assert(typing_store.feeds()[0].title=="nirss");typing.back();
    typing.submit();assert(typing_store.feeds()[0].title=="nirss");
}
