#include "render.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(){
    const char* path=std::getenv("RSS_FONT");
    std::string font=path?path:"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Canvas c(1072,1448,font);
    // Every action icon paints within its box, including clipped screen-edge draws.
    for(int value=static_cast<int>(rss::Icon::Rss);value<=static_cast<int>(rss::Icon::Check);++value){
        c.clear();c.icon(static_cast<rss::Icon>(value),{20,20,36,36});int painted=0;
        for(int y=0;y<c.height;++y)for(int x=0;x<c.width;++x)if(c.pixels[y*c.width+x]!=255){
            assert(x>=20&&x<56&&y>=20&&y<56);++painted;
        }
        assert(painted>30);c.icon(static_cast<rss::Icon>(value),{-10,-10,36,36});
    }
    c.clear();
    std::string text;
    for(int i=0;i<100;++i)text+="中文分页测试。A paragraph with words and punctuation, safe UTF-8 wrapping.\n\n";
    auto pages=c.paginate(text,32,52,944,1100);assert(pages.size()>2);
    std::size_t cursor=0;
    for(const auto& p:pages){
        assert(p.start==cursor);for(const auto& line:p.lines){assert(line.start==cursor);cursor=line.end;assert(c.measure(line.text,32)<=944);}
        assert(p.end==cursor);
    }
    assert(cursor==text.size());
    const auto short_first=c.paginate(text,32,52,944,1100,260);
    assert(short_first.front().lines.size()==5&&short_first[1].lines.size()==21);
    std::size_t covered=0;
    for(const auto& page:short_first){assert(page.start==covered);covered=page.end;}
    assert(covered==text.size());
    auto offset=pages[2].start;auto resized=c.paginate(text,40,60,944,1100);
    auto index=rss::page_at(resized,offset);assert(resized[index].start<=offset&&offset<resized[index].end);
    assert(c.wrap("\n\n",32,200).size()==2);
    assert(!c.wrap("",32,200).empty());
    assert(c.measure(c.ellipsize("这是一段特别长的标题",32,180),32)<=180);
    assert(!c.wrap(std::string("broken\xff",7),32,200).empty());
    // Images stay in document order, fit the page, and keep their short caption.
    const std::string article="开头一段。\n图片说明\n后文。";
    const auto at=article.find("图片说明"),after=article.find("后文");
    const auto flow=c.paginate_article(article,{{at,after,0,600,900},{after,after,1,600,900}},32,52,944,1203,900);
    std::string reconstructed;int image_count=0;
    for(std::size_t i=0;i<flow.size();++i){
        const auto& page=flow[i];int bottom=0;
        for(const auto& item:page.elements){
            assert(item.rect.y>=bottom);bottom=item.rect.y+item.rect.h;
            assert(bottom<=(i==0?900:1203));assert(item.rect.x>=0&&item.rect.x+item.rect.w<=944);
            if(item.image>=0){assert(item.image==image_count++);}
            else reconstructed+=article.substr(item.line.start,item.line.end-item.line.start);
        }
        if(!page.elements.empty()&&page.elements.front().image==0){
            assert(page.elements.size()>1&&page.elements[1].caption);
        }
    }
    assert(image_count==2&&reconstructed==article);
    const auto photos=c.paginate_article("",{{0,0,0,600,900},{0,0,1,600,900},{0,0,0,600,900}},32,52,944,1203,900);
    assert(photos.size()==3);
    for(std::size_t i=0;i<photos.size();++i){assert(photos[i].image_anchor==int(i));assert(photos[i].elements[0].rect.h<=(i==0?868:900));}
    const auto missing=c.paginate_article("正文",{{0,0,0,0,0}},48,78,944,1203,900);
    assert(missing.front().elements.front().rect.h==120);
    c.fill({-100,-100,120,120},0);assert(c.pixels[0]==0);assert(c.pixels[30]==255);
    std::cout<<"render checks passed\n";
}
