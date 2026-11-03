#include "app.hpp"
#include "feed.hpp"
#include "translation.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
namespace {
void hold(rss::App& app,rss::TouchPoint p){rss::TouchGesture g(p);g.phase=rss::TouchPhase::LongPress;g.hold_ms=700;app.touch(g);g.phase=rss::TouchPhase::Complete;app.touch(g);}
rss::TouchPoint pick(rss::App& app,const std::string& word){
    for(int y=110;y<850;y+=12)for(int x=38;x<1000;x+=12){hold(app,{x,y});if(app.selected_text()){if(app.selected_text()->range.text==word)return {x,y};app.back();}}
    assert(false);return {};
}
rss::Rect toolbar(const rss::Canvas& c){
    for(int y=96;y<c.height-151;++y)for(int x=32;x<=c.width-272;++x){
        const auto at=[&](int dx,int dy){return c.pixels[static_cast<std::size_t>(y+dy)*c.width+x+dx];};
        if(at(0,0)!=30||at(-1,0)==30||at(239,0)!=30||at(0,79)!=30||at(239,79)!=30)continue;
        bool border=true;for(int dx=0;dx<240;++dx)if(at(dx,0)!=30||at(dx,79)!=30){border=false;break;}
        if(border)return {x,y,240,80};
    }
    assert(false&&"manual translation button missing");return {};
}
void translate(rss::App& app,rss::Canvas& c){app.draw();const auto r=toolbar(c);app.tap(r.x+r.w/2,r.y+r.h/2);}
}
int main(){
    const std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):
        std::filesystem::exists("/System/Library/Fonts/STHeiti Light.ttc")?"/System/Library/Fonts/STHeiti Light.ttc":"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    for(const auto* engine:{"classic","cre"})for(bool web:{false,true}){
        if(std::string(engine)=="cre"&&!rss::CreReader::available())continue;
        rss::Store store(":memory:");store.set_setting("reader_engine",engine);rss::Canvas c(1072,1448,font);
        rss::Feed f;f.url="https://selection.invalid/feed";auto fid=store.add_feed(f);
        rss::Entry e;e.guid="one";e.title="Manual translation";e.url="https://selection.invalid/story";e.html="<p>of a tiny example.</p>";for(int i=0;i<40;++i)e.html+="<p>English readers select useful words and complete sentences.</p>";e.text=rss::html_to_text(e.html);
        rss::ParsedFeed parsed;parsed.entries={e};store.save_entries(fid,parsed);auto id=store.entries(rss::View::All).front().id;e=*store.entry(id);
        if(web){store.queue_fulltext(id);store.save_fulltext(id,e);e=*store.fulltext_entry(id);}
        store.set_setting("reader_content:"+std::to_string(id),web?"web":"rss");rss::App app(store,c);app.open(id);app.draw();
        const auto point=pick(app,"English");assert(app.take_request().empty()&&!app.take_selection_request());
        for(int i=0;i<5;++i)app.draw();assert(app.take_request().empty()&&app.reader_panel()==rss::App::ReaderPanel::None);
        // The finger may leave the glyphs at the end of a long drag.
        const auto end_point=pick(app,"sentences");app.back();hold(app,point);
        rss::TouchGesture edge(point);edge.update({1070,end_point.y});edge.phase=rss::TouchPhase::Move;edge.hold_ms=700;app.touch(edge);
        assert(app.selected_text()&&app.selected_text()->range.text.find("sentences")!=std::string::npos);
        edge.phase=rss::TouchPhase::Complete;app.touch(edge);assert(app.reader_page()==0&&app.take_request().empty());
        app.back();hold(app,end_point);
        rss::TouchGesture reverse(end_point);reverse.update({1,point.y});reverse.phase=rss::TouchPhase::Move;reverse.hold_ms=700;app.touch(reverse);
        assert(app.selected_text()&&app.selected_text()->range.text.find("English")!=std::string::npos&&app.selected_text()->range.text.find("sentences")!=std::string::npos);
        app.back();hold(app,point);
        rss::TouchGesture bottom(point);bottom.update({1070,1390});bottom.phase=rss::TouchPhase::Move;bottom.hold_ms=700;app.touch(bottom);
        assert(app.selected_text()&&app.selected_text()->rects.back().y>900&&app.reader_page()==0&&app.take_request().empty());
        app.back();hold(app,{2,150});assert(!app.selected_text());
        rss::TouchGesture blank({2,150});blank.update(end_point);blank.phase=rss::TouchPhase::Move;blank.hold_ms=700;app.touch(blank);assert(!app.selected_text());
        const auto useful=pick(app,"useful");app.back();const auto words=pick(app,"words");app.back();
        for(bool first:{true,false}){
            hold(app,point);rss::TouchGesture range(point);range.update(end_point);range.phase=rss::TouchPhase::Move;range.hold_ms=700;app.touch(range);app.draw();
            const auto rect=first?app.selected_text()->rects.front():app.selected_text()->rects.back();
            const rss::TouchPoint handle{32+rect.x+(first?36:rect.w-36),96+rect.y+rect.h+8};
            rss::TouchGesture move(handle);move.phase=rss::TouchPhase::Move;move.hold_ms=700;move.update(first?useful:words);app.touch(move);
            move.update(first?words:useful);app.touch(move);move.phase=rss::TouchPhase::Complete;app.touch(move);
            assert(app.selected_text());const auto text=app.selected_text()->range.text;
            assert(text.find(first?"words":"English")==0&&text.find(first?"sentences":"useful")!=std::string::npos);
            assert(app.reader_page()==0&&app.take_request().empty());app.back();
        }
        const auto of=pick(app,"of");app.back();const auto a_word=pick(app,"a");app.back();const auto tiny=pick(app,"tiny");app.back();const auto example=pick(app,"example");app.back();
        hold(app,of);rss::TouchGesture short_range(of);short_range.update(a_word);short_range.phase=rss::TouchPhase::Move;short_range.hold_ms=700;app.touch(short_range);app.draw();
        const auto short_tail=app.selected_text()->rects.back();
        rss::TouchGesture overlap({32+short_tail.x+short_tail.w-24,96+short_tail.y+short_tail.h+8});overlap.phase=rss::TouchPhase::Move;overlap.hold_ms=700;overlap.update(tiny);app.touch(overlap);overlap.update(example);app.touch(overlap);overlap.phase=rss::TouchPhase::Complete;app.touch(overlap);
        assert(app.selected_text()&&app.selected_text()->range.text.find("of a tiny example")==0&&app.take_request().empty());app.back();
        hold(app,point);
        const auto r=toolbar(c);assert(r.y+r.h<96+app.selected_text()->rects.front().y||r.y>96+app.selected_text()->rects.back().y);
        translate(app,c);assert(app.take_request()=="translate-selection");auto job=app.take_selection_request();assert(job&&job->range.text=="English");
        assert(app.take_request().empty()&&app.reader_panel()==rss::App::ReaderPanel::Selection);
        store.save_selection_translation(*job,"英语");app.selection_finished(*job,"英语",false);app.back();app.draw();
        hold(app,point);app.draw();assert(app.take_request().empty()&&app.reader_panel()==rss::App::ReaderPanel::None);translate(app,c);assert(app.take_request().empty()&&app.reader_panel()==rss::App::ReaderPanel::Selection);
        app.back();pick(app,"readers");assert(app.take_request().empty());translate(app,c);assert(app.take_request()=="translate-selection");auto failed=app.take_selection_request();assert(failed);app.selection_finished(*failed,"",false,"连接失败");app.draw();assert(app.take_request().empty());app.tap(536,1304);assert(app.take_request()=="translate-selection");auto retry=app.take_selection_request();assert(retry);app.selection_finished(*retry,"",true);app.back();
        // Original reading controls and result panel stay unchanged.
        hold(app,point);app.draw();app.tap(50,1000);assert(!app.selected_text()&&app.reader_page()==0&&app.take_request().empty());
        const auto doc=rss::translation_document(e);std::vector<std::string> blocks(doc.blocks.size(),"中文 example 译文");store.begin_translation(id,web,doc.revision,blocks.size());for(std::size_t i=0;i<blocks.size();++i)store.save_translation_block(id,web,doc.revision,i,blocks[i]);store.set_translation_state(id,web,doc.revision,"ready");
        for(auto mode:{rss::ReadingMode::Translated,rss::ReadingMode::Bilingual}){
            store.set_setting(std::string("reader_mode:")+(web?"web:":"rss:")+std::to_string(id),mode==rss::ReadingMode::Translated?"translated":"bilingual");app.open(id);pick(app,"example");assert(app.take_request().empty());translate(app,c);assert(app.take_request()=="translate-selection");auto translated=app.take_selection_request();assert(translated&&translated->mode==mode);app.selection_finished(*translated,"例子",false);app.back();
        }
    }
    std::cout<<"Only manual Translate button; long press, release, draw and cache never auto-translate; classic/CRE RSS/web modes passed\n";
}
