#include "cre_reader.hpp"
#include "feed.hpp"
#include "render.hpp"
#include "store.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace {
struct Hit { int x, y; rss::ReaderSelection selection; };
Hit find_word(rss::CreReader& reader,rss::Canvas& canvas,std::size_t page,const std::string& word) {
    canvas.clear();reader.draw(canvas,page,0,0,canvas.height);
    for(int y=0;y<canvas.height;++y)for(int x=0;x<canvas.width;++x) {
        if(canvas.pixels[static_cast<std::size_t>(y)*canvas.width+x]>160)continue;
        auto selected=reader.select(page,x,y,x,y);
        if(selected&&selected->range.text==word)return {x,y,std::move(*selected)};
    }
    assert(false&&"fixture word was not found by scanning rendered glyph pixels");
    return {};
}
Hit find_chinese_hit(rss::CreReader& reader,rss::Canvas& canvas,std::size_t page) {
    canvas.clear();reader.draw(canvas,page,0,0,canvas.height);
    for(int y=0;y<canvas.height;++y)for(int x=0;x<canvas.width;++x) {
        if(canvas.pixels[static_cast<std::size_t>(y)*canvas.width+x]>160)continue;
        auto selected=reader.select(page,x,y,x,y);
        if(selected&&selected->range.text.find("词")!=std::string::npos)return {x,y,std::move(*selected)};
    }
    assert(false&&"fixture Chinese glyph was not found by scanning rendered pixels");
    return {};
}
}

int main() {
    assert(rss::CreReader::available());
    const auto configured=std::getenv("RSS_FONT");
    const std::string font=configured?configured:"/System/Library/Fonts/STHeiti Light.ttc";
    rss::Store store(":memory:");
    rss::Canvas canvas(1072,1448,font);
    rss::Entry article;
    article.title="Selection fixture title";
    article.html="<p>Alpha <a href='https://fixture.invalid/'>linked</a> <strong>emphasis</strong> and 中文词语。</p>"
        "<p>beginning cross-line selection carries enough words to wrap around the narrow reading area and continues here.</p>"
        "<p><img src='https://fixture.invalid/missing.png' alt='missing'/>Visible after image.</p>";
    for(int i=0;i<90;++i)
        article.html+="<p>Paragraph "+std::to_string(i)+" keeps persistent selection anchors tied to document content after the layout width and font size change.</p>";
    article.text=rss::html_to_text(article.html);
    rss::CreReader reader(store,font);
    reader.open(article,"https://fixture.invalid/article","feed metadata",760,1000,34,24);
    assert(reader.page_count()>1);

    const auto alpha=find_word(reader,canvas,0,"Alpha");
    auto same=reader.select(0,alpha.x,alpha.y,alpha.x,alpha.y);
    assert(same&&same->range.text=="Alpha"&&!same->rects.empty());
    assert(same->range.start.find(':')!=std::string::npos&&same->range.end.find(':')!=std::string::npos);
    const auto linked=find_word(reader,canvas,0,"linked");
    const auto emphasized=find_word(reader,canvas,0,"emphasis");
    assert(linked.selection.range.text=="linked"&&emphasized.selection.range.text=="emphasis");
    assert(!reader.select(0,-1,alpha.y,alpha.x,alpha.y));
    assert(!reader.select(reader.page_count(),alpha.x,alpha.y,alpha.x,alpha.y));
    assert(!reader.select(0,0,0,0,0));

    const auto beginning=find_word(reader,canvas,0,"beginning");
    const auto continues=find_word(reader,canvas,0,"continues");
    auto forward=reader.select(0,beginning.x,beginning.y,continues.x,continues.y);
    auto reverse=reader.select(0,continues.x,continues.y,beginning.x,beginning.y);
    assert(forward&&reverse&&forward->range.text==reverse->range.text);
    assert(forward->range.text.find("cross-line selection")!=std::string::npos);
    // Ending a drag in line spacing should still include the nearest visible word.
    const auto end_rect=continues.selection.rects.back();
    const auto in_spacing=reader.select(0,beginning.x,beginning.y,end_rect.x+end_rect.w/2,end_rect.y+end_rect.h+6,true);
    assert(in_spacing&&in_spacing->range.text.find("continues")!=std::string::npos);
    const auto reverse_spacing=reader.select(0,end_rect.x+end_rect.w/2,end_rect.y+end_rect.h+6,beginning.x,beginning.y,true);
    assert(reverse_spacing&&reverse_spacing->range.text==in_spacing->range.text);
    assert(!reader.select(0,beginning.x,beginning.y,end_rect.x+end_rect.w/2,end_rect.y+end_rect.h+6));
    const auto row_end=reader.select(0,beginning.x,beginning.y,10000,continues.y+continues.selection.rects.front().h/2,true);
    assert(row_end&&row_end->range.text.find("continues")!=std::string::npos);
    const auto clamped=reader.select(0,beginning.x,beginning.y,-10000,2000,true);
    assert(clamped&&clamped->range.text.find("continues")!=std::string::npos);
    const auto persistent_rects=reader.selection_rects(0,forward->range);
    assert(!persistent_rects.empty()&&persistent_rects.size()==forward->rects.size());

    const auto chinese=find_chinese_hit(reader,canvas,0);
    auto chinese_selection=reader.select(0,chinese.x,chinese.y,chinese.x,chinese.y);
    assert(chinese_selection&&chinese_selection->range.text.find("词")!=std::string::npos);
    auto invalid=forward->range;invalid.start="old-revision:"+invalid.start;
    assert(reader.selection_rects(0,invalid).empty());

    auto persistent=forward->range;
    reader.layout(620,1000,42,24);
    const auto reflow_page=reader.restore(reader.anchor(0));
    (void)reflow_page;
    const auto reflow_beginning=find_word(reader,canvas,0,"beginning");
    const auto reflow_continues=find_word(reader,canvas,0,"continues");
    const auto reflow_adjusted=reader.select(0,reflow_beginning.x,reflow_beginning.y,reflow_continues.x,
        reflow_continues.y+reflow_continues.selection.rects.front().h+6,true);
    assert(reflow_adjusted&&reflow_adjusted->range.text.find("continues")!=std::string::npos);
    auto reflow_rects=reader.selection_rects(0,persistent);
    assert(!reflow_rects.empty());
    for(const auto& rect:reflow_rects)assert(rect.x>=0&&rect.y>=0&&rect.x+rect.w<=620&&rect.y+rect.h<=1000);
    reader.open(article,"https://fixture.invalid/article","updated source metadata",620,1000,42,24);
    assert(!reader.selection_rects(0,persistent).empty());
    const auto reopened=reader.select(0,reflow_beginning.x,reflow_beginning.y,reflow_continues.x,
        reflow_continues.y+reflow_continues.selection.rects.front().h+6,true);
    assert(reopened&&reopened->range.text.find("continues")!=std::string::npos);
    reader.layout(300,320,42,24);
    int visible_pages=0;
    for(std::size_t page=0;page<reader.page_count();++page){
        const auto rectangles=reader.selection_rects(page,persistent);if(!rectangles.empty())++visible_pages;
        for(const auto& rect:rectangles)assert(rect.x>=0&&rect.y>=0&&rect.x+rect.w<=300&&rect.y+rect.h<=320);
    }
    assert(visible_pages>=2);reader.layout(620,1000,42,24);
    auto changed=article;changed.html="<p>Changed source body.</p>";changed.text=rss::html_to_text(changed.html);
    reader.open(changed,"https://fixture.invalid/article","feed metadata",620,1000,42,24);
    assert(reader.selection_rects(0,persistent).empty());

    std::cout<<"CREngine word selection, range anchors and reflow geometry passed\n";
}
