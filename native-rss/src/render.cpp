#include "render.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <cmath>
#include FT_MULTIPLE_MASTERS_H

namespace rss {
namespace {
struct Character { std::uint32_t cp; std::size_t begin, end; };
std::vector<Character> characters(const std::string& s) {
    std::vector<Character> out;
    for (std::size_t i=0; i<s.size();) {
        const auto start=i; const auto c=static_cast<unsigned char>(s[i++]);
        std::uint32_t cp=c; int extra=0;
        if(c>=0xc2 && c<=0xdf) {cp=c&0x1f; extra=1;}
        else if(c>=0xe0 && c<=0xef) {cp=c&0x0f; extra=2;}
        else if(c>=0xf0 && c<=0xf4) {cp=c&0x07; extra=3;}
        else if(c>=0x80) cp=0xfffd;
        bool valid=i+extra<=s.size();
        for(int k=0;valid && k<extra;++k) {
            const auto b=static_cast<unsigned char>(s[i+k]);
            if((b&0xc0)!=0x80) valid=false;
            else cp=(cp<<6)|(b&0x3f);
        }
        if (extra && (cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) ||
            (extra==1 && cp<0x80) || (extra==2 && cp<0x800) || (extra==3 && cp<0x10000))) valid=false;
        if(valid) i+=extra; else cp=0xfffd;
        out.push_back({cp,start,i});
    }
    return out;
}
bool no_line_start(std::uint32_t c) {
    const std::u32string chars=U"，。！？：；、）》】」』”’,.!?;:)]}";
    return chars.find(c)!=std::u32string::npos;
}
bool no_line_end(std::uint32_t c) {
    return std::u32string(U"（《【「『“‘([{ ").find(c)!=std::u32string::npos;
}
}
bool Rect::contains(int px,int py) const {return px>=x&&py>=y&&px<x+w&&py<y+h;}
Canvas::Canvas(int w,int h,const std::string& font):width(w),height(h),font_path_(font) {
    if(w<100||h<100||w>4096||h>4096) throw std::runtime_error("无效屏幕尺寸");
    pixels.assign(static_cast<std::size_t>(w)*h,255);
    if(FT_Init_FreeType(&library_)) throw std::runtime_error("字体引擎初始化失败");
    if(FT_New_Face(library_,font.c_str(),0,&face_)) {
        FT_Done_FreeType(library_); library_=nullptr;
        throw std::runtime_error("无法打开中文字体："+font);
    }
    try{
        if(FT_HAS_MULTIPLE_MASTERS(face_)){
            FT_MM_Var* raw=nullptr;
            if(FT_Get_MM_Var(face_,&raw))throw std::runtime_error("无法读取可变字体参数");
            auto release=[this](FT_MM_Var* value){FT_Done_MM_Var(library_,value);};
            std::unique_ptr<FT_MM_Var,decltype(release)> variation(raw,release);
            std::vector<FT_Fixed> axes(raw->num_axis);
            for(FT_UInt i=0;i<raw->num_axis;++i){
                const auto& axis=raw->axis[i];axes[i]=axis.def;
                if(axis.tag==FT_MAKE_TAG('w','g','h','t'))axes[i]=std::clamp<FT_Fixed>(400L<<16,axis.minimum,axis.maximum);
            }
            if(FT_Set_Var_Design_Coordinates(face_,raw->num_axis,axes.data()))throw std::runtime_error("无法设置字体字重");
        }
    }catch(...){FT_Done_Face(face_);FT_Done_FreeType(library_);face_=nullptr;library_=nullptr;throw;}
}
Canvas::~Canvas(){if(face_)FT_Done_Face(face_);if(library_)FT_Done_FreeType(library_);}
const Glyph& Canvas::glyph(std::uint32_t cp,int size) {
    size=std::clamp(size,12,100);
    const auto key=std::make_pair(size,cp);
    auto found=cache_.find(key); if(found!=cache_.end())return found->second;
    if(cache_.size()>10000)cache_.clear();
    if(FT_Set_Pixel_Sizes(face_,0,size) || FT_Load_Char(face_,cp,FT_LOAD_RENDER))
        throw std::runtime_error("文字渲染失败");
    const auto& bitmap=face_->glyph->bitmap;
    Glyph g;g.width=bitmap.width;g.height=bitmap.rows;g.left=face_->glyph->bitmap_left;
    g.top=face_->glyph->bitmap_top;g.advance=static_cast<int>(face_->glyph->advance.x>>6);
    g.pixels.resize(static_cast<std::size_t>(g.width)*g.height);
    for(int y=0;y<g.height;++y)for(int x=0;x<g.width;++x) {
        const auto row=bitmap.buffer+(bitmap.pitch>=0?y:g.height-1-y)*std::abs(bitmap.pitch);
        g.pixels[y*g.width+x]=bitmap.pixel_mode==FT_PIXEL_MODE_MONO ? ((row[x/8]&(0x80>>(x%8)))?255:0):row[x];
    }
    return cache_.emplace(key,std::move(g)).first->second;
}
void Canvas::clear(std::uint8_t color){std::fill(pixels.begin(),pixels.end(),color);}
void Canvas::fill(Rect r,std::uint8_t color){
    const int left=std::clamp(r.x,0,width),right=std::clamp(r.x+r.w,0,width);
    if(right<=left)return;
    for(int y=std::max(0,r.y);y<std::min(height,r.y+r.h);++y)
        std::fill(pixels.begin()+y*width+left,pixels.begin()+y*width+right,color);
}
// Icons use one 24-unit grid; status symbols have thinner strokes than controls.
void Canvas::icon(Icon kind,Rect area,std::uint8_t color){
    if(kind==Icon::None||area.w<=0||area.h<=0)return;
    const auto right=static_cast<std::int64_t>(area.x)+area.w;
    const auto bottom=static_cast<std::int64_t>(area.y)+area.h;
    const int left=std::max(0,area.x),top=std::max(0,area.y);
    const int clipped_right=static_cast<int>(std::min<std::int64_t>(width,right));
    const int clipped_bottom=static_cast<int>(std::min<std::int64_t>(height,bottom));
    if(left>=clipped_right||top>=clipped_bottom)return;
    auto composite=[&](const std::vector<std::uint16_t>& mask){
        for(int y=top;y<clipped_bottom;++y)for(int x=left;x<clipped_right;++x){
            const auto dx=static_cast<std::size_t>(static_cast<std::int64_t>(x)-area.x);
            const auto dy=static_cast<std::size_t>(static_cast<std::int64_t>(y)-area.y);
            const std::uint32_t alpha=mask[dy*area.w+dx];
            if(!alpha)continue;
            auto& pixel=pixels[y*width+x];
            pixel=static_cast<std::uint8_t>((static_cast<std::uint32_t>(pixel)*(65535-alpha)+
                                             static_cast<std::uint32_t>(color)*alpha)/65535);
        }
    };
    const auto key=std::make_tuple(kind,area.w,area.h);
    if(area.w<=128&&area.h<=128){
        const auto found=icon_coverage_cache_.find(key);
        if(found!=icon_coverage_cache_.end()){composite(found->second);return;}
    }
    struct Segment { double x1,y1,x2,y2; };
    std::vector<Segment> lines;
    auto line=[&](double x,double y,double xx,double yy){lines.push_back({x,y,xx,yy});};
    auto arc=[&](double x,double y,double radius,double from,double to){
        for(int i=0;i<32;++i){double a=from+(to-from)*i/32,b=from+(to-from)*(i+1)/32;
            line(x+radius*std::cos(a),y+radius*std::sin(a),x+radius*std::cos(b),y+radius*std::sin(b));}
    };
    const double pi=3.141592653589793;
    auto box=[&](double x,double y,double w,double h){line(x,y,x+w,y);line(x+w,y,x+w,y+h);line(x+w,y+h,x,y+h);line(x,y+h,x,y);};
    switch(kind){
    case Icon::Rss: arc(5,19,7,-pi/2,0);arc(5,19,14,-pi/2,0);arc(5,19,1,0,2*pi);break;
    case Icon::Search:arc(10,10,6,0,2*pi);line(15,15,21,21);break;
    case Icon::Sync:
        arc(12,12,8,0,pi*1.75);line(17.66,6.34,20,8);
        line(20,3,20,8);line(20,8,15,8);break;
    case Icon::Star:case Icon::StarFilled:for(int i=0;i<10;++i){double a=-pi/2+i*pi/5,b=a+pi/5,r=i%2?4:9,rr=i%2?9:4;line(12+r*std::cos(a),12+r*std::sin(a),12+rr*std::cos(b),12+rr*std::sin(b));}break;
    case Icon::Back:line(14,5,7,12);line(7,12,14,19);break;
    case Icon::Next:line(10,5,17,12);line(17,12,10,19);break;
    case Icon::Close:line(6,6,18,18);line(18,6,6,18);break;
    case Icon::Book:box(3,5,9,15);box(12,5,9,15);line(6,9,9,9);line(15,9,18,9);break;
    case Icon::Clock:arc(12,12,9,0,2*pi);line(12,6,12,12);line(12,12,16,14);break;
    case Icon::Inbox:box(4,4,16,16);line(4,14,9,14);line(9,14,9,17);line(9,17,15,17);line(15,17,15,14);line(15,14,20,14);break;
    case Icon::Folder:line(3,7,3,20);line(3,20,21,20);line(21,20,21,7);line(21,7,12,7);line(12,7,9,4);line(9,4,3,4);line(3,4,3,7);break;
    case Icon::Plus:line(12,4,12,20);line(4,12,20,12);break;
    case Icon::Settings:for(int i=0;i<3;++i){int y=5+i*7,x=i==1?15:8;line(3,y,x-2,y);line(x+2,y,21,y);arc(x,y,2,0,2*pi);}break;
    case Icon::Image:box(3,4,18,16);arc(8,9,1,0,2*pi);line(4,18,11,11);line(11,11,15,15);line(15,15,18,12);line(18,12,21,15);break;
    case Icon::Download:case Icon::Upload:{bool down=kind==Icon::Download;int tip=down?15:4,tail=down?4:15,wing=down?10:9;line(12,tail,12,tip);line(7,wing,12,tip);line(12,tip,17,wing);line(4,16,4,21);line(4,21,20,21);line(20,21,20,16);break;}
    case Icon::Exit:line(10,4,4,4);line(4,4,4,20);line(4,20,10,20);line(10,12,21,12);line(16,7,21,12);line(21,12,16,17);break;
    case Icon::Check:line(4,12,9,17);line(9,17,20,6);break;
    case Icon::Info:arc(12,12,9,0,2*pi);arc(12,7,0.5,0,2*pi);line(12,11,12,17);break;
    case Icon::Battery:
        line(3,7,19,7);arc(19,8,1,-pi/2,0);line(20,8,20,15.5);arc(19,15.5,1,0,pi/2);
        line(19,16.5,3,16.5);arc(3,15.5,1,pi/2,pi);line(2,15.5,2,8);arc(3,8,1,pi,pi*1.5);
        line(20,11,21.5,11);line(20,12,21.5,12);line(20,13,21.5,13);break;
    case Icon::Wifi:arc(12,20,3,pi*1.25,pi*1.75);arc(12,20,7,pi*1.25,pi*1.75);arc(12,20,11,pi*1.25,pi*1.75);arc(12,20,.6,0,2*pi);break;
    case Icon::BackCircle:arc(12,12,10,0,2*pi);line(14,8,10,12);line(10,12,14,16);break;
    case Icon::None:break;
    }
    const double scale=std::min(area.w,area.h)/24.0;
    const double ox=area.x+(area.w-24*scale)/2,oy=area.y+(area.h-24*scale)/2;
    struct PreparedSegment { double x1,y1,dx,dy,inverse_length; };
    std::vector<PreparedSegment> segments;segments.reserve(lines.size());
    for(const auto& l:lines){const double dx=l.x2-l.x1,dy=l.y2-l.y1,length=dx*dx+dy*dy;
        segments.push_back({l.x1,l.y1,dx,dy,length?1.0/length:0.0});}
    auto coverage=[&](double px,double py){
            double distance=10000;
            for(const auto& l:segments){
                const double t=std::clamp(((px-l.x1)*l.dx+(py-l.y1)*l.dy)*l.inverse_length,0.0,1.0);
                const double ex=px-l.x1-t*l.dx,ey=py-l.y1-t*l.dy;
                distance=std::min(distance,ex*ex+ey*ey);}
            distance=std::sqrt(distance);
            bool inside=false;
            if(kind==Icon::StarFilled)for(const auto& edge:lines){
                if((edge.y1>py)!=(edge.y2>py) &&
                   px<(edge.x2-edge.x1)*(py-edge.y1)/(edge.y2-edge.y1)+edge.x1)inside=!inside;
            }
            const double stroke=kind==Icon::Battery?.5:kind==Icon::Wifi?.85:1;
            return std::clamp((stroke+(inside?distance:-distance))*scale+.5,0.0,1.0);
    };
    auto paint=[&](int x,int y,double alpha){
            auto& pixel=pixels[y*width+x];pixel=static_cast<std::uint8_t>(pixel*(1-alpha)+color*alpha);
    };
    if(area.w<=128&&area.h<=128){
        // Cache coverage only; integer blending preserves arbitrary backgrounds and ink.
        constexpr std::size_t max_entries=(128*1024)/sizeof(std::uint16_t);
        std::vector<std::uint16_t> mask(static_cast<std::size_t>(area.w)*area.h);
        const double offset_x=(area.w-24*scale)/2,offset_y=(area.h-24*scale)/2;
        for(int y=0;y<area.h;++y)for(int x=0;x<area.w;++x){
            const auto alpha=coverage((x+.5-offset_x)/scale,(y+.5-offset_y)/scale);
            mask[static_cast<std::size_t>(y)*area.w+x]=static_cast<std::uint16_t>(std::lround(alpha*65535.0));
        }
        if(icon_coverage_entries_+mask.size()>max_entries){icon_coverage_cache_.clear();icon_coverage_entries_=0;}
        const auto found=icon_coverage_cache_.emplace(key,std::move(mask)).first;
        icon_coverage_entries_+=found->second.size();
        composite(found->second);
        return;
    }
    for(int y=top;y<clipped_bottom;++y)for(int x=left;x<clipped_right;++x)
        paint(x,y,coverage((x+.5-ox)/scale,(y+.5-oy)/scale));
}
void Canvas::rule(int x,int y,int w,std::uint8_t color){fill({x,y,w,1},color);}
int Canvas::text(int x,int baseline,const std::string& value,int size,std::uint8_t color){
    const int start=x;
    for(const auto& c:characters(value)){
        if(c.cp=='\n'||c.cp=='\r')continue;
        const auto& g=glyph(c.cp,size);
        for(int y=0;y<g.height;++y)for(int gx=0;gx<g.width;++gx){
            const int dx=x+g.left+gx,dy=baseline-g.top+y;
            if(dx<0||dx>=width||dy<0||dy>=height)continue;
            const unsigned a=g.pixels[y*g.width+gx];auto& p=pixels[dy*width+dx];
            p=static_cast<std::uint8_t>((p*(255-a)+color*a)/255);
        }
        x+=g.advance;
    }
    return x-start;
}
std::pair<int,int> Canvas::text_vertical_bounds(const std::string& value,int size){
    int top=0,bottom=0;bool visible=false;
    for(const auto& c:characters(value)){
        if(c.cp=='\n'||c.cp=='\r')continue;
        const auto& g=glyph(c.cp,size);if(!g.width||!g.height)continue;
        top=visible?std::min(top,-g.top):-g.top;
        bottom=visible?std::max(bottom,g.height-g.top):g.height-g.top;visible=true;
    }
    return {top,bottom};
}
int Canvas::measure(const std::string& s,int size){int w=0;for(auto c:characters(s))w+=glyph(c.cp,size).advance;return w;}
std::vector<Line> Canvas::wrap(const std::string& s,int size,int max_width){
    if(max_width<size)throw std::runtime_error("排版宽度太小");
    auto cs=characters(s);std::vector<Line> lines;
    for(std::size_t start=0;start<cs.size();){
        std::size_t end=start,last_break=start;int w=0;
        while(end<cs.size()&&cs[end].cp!='\n'){
            const int advance=glyph(cs[end].cp,size).advance;
            if(w+advance>max_width && end>start)break;
            w+=advance;
            if(cs[end].cp==' '||cs[end].cp=='\t')last_break=end+1;
            ++end;
        }
        if(end<cs.size()&&cs[end].cp!='\n'){
            if(last_break>start && cs[end].cp<128 && cs[end-1].cp<128)end=last_break;
            // Keep closing punctuation with its predecessor, without overflowing the column.
            if(end>start+1 && no_line_start(cs[end].cp))--end;
            if(end>start+1 && no_line_end(cs[end-1].cp))--end;
        }
        auto from=cs[start].begin;
        auto to=end<cs.size()?cs[end].begin:s.size();
        const auto visible_to=to;
        if(end<cs.size()&&cs[end].cp=='\n'){to=cs[end].end;++end;}
        lines.push_back({s.substr(from,visible_to-from),from,to});
        start=end;
    }
    if(lines.empty())lines.push_back({"",0,0});
    return lines;
}
std::vector<Page> Canvas::paginate(const std::string& s,int size,int lh,int w,int h,int first_height){
    if(lh<size||h<lh)throw std::runtime_error("无效分页参数");
    const auto lines=wrap(s,size,w);const auto count=static_cast<std::size_t>(h/lh);
    std::vector<Page> pages;
    for(std::size_t i=0;i<lines.size();){
        const auto capacity=i==0&&first_height>0?static_cast<std::size_t>(std::max(lh,first_height)/lh):count;
        const auto end=std::min(i+capacity,lines.size());
        pages.push_back({std::vector<Line>(lines.begin()+i,lines.begin()+end),lines[i].start,lines[end-1].end});
        i=end;
    }
    return pages;
}
std::vector<Page> Canvas::paginate_article(const std::string& value,const std::vector<InlineImage>& images,
                                           int size,int lh,int w,int h,int first_height){
    if(size<1||lh<size||w<1||h<lh||first_height<lh)throw std::runtime_error("无效文章分页参数");
    std::vector<Page> pages(1);
    int y=0,available=first_height;
    auto room=[&](int height){
        if(y+height>available&&!pages.back().elements.empty()){
            pages.emplace_back();y=0;available=h;
        }
    };
    auto add=[&](PageElement item){
        auto& page=pages.back();
        if(page.elements.empty()){page.start=item.line.start;page.image_anchor=item.occurrence;}
        page.end=item.line.end;page.elements.push_back(std::move(item));
    };
    auto text_range=[&](std::size_t begin,std::size_t end,bool caption){
        if(begin==end)return;
        const int fs=caption?std::max(22,size-6):size,step=caption?fs+12:lh;
        for(auto line:wrap(value.substr(begin,end-begin),fs,w)){
            line.start+=begin;line.end+=begin;
            room(step);
            // Paragraph boundaries add breathing room without inventing text or changing stored offsets.
            PageElement item;item.line=line;item.rect={0,y,w,step};item.size=fs;item.caption=caption;
            add(std::move(item));y+=step;
            if(!caption&&line.end>line.start&&value[line.end-1]=='\n')y+=12;
        }
    };
    std::size_t cursor=0;
    for(std::size_t n=0;n<images.size();++n){
        const auto& image=images[n];
        if(image.offset<cursor||image.offset>value.size()||image.caption_end<image.offset||image.caption_end>value.size())
            throw std::runtime_error("无效图片正文位置");
        text_range(cursor,image.offset,false);
        const int caption_size=std::max(22,size-6);
        const int caption_lines=image.caption_end>image.offset?static_cast<int>(wrap(value.substr(image.offset,image.caption_end-image.offset),caption_size,w).size()):0;
        const int reserve=std::min(3,caption_lines)*(caption_size+12);
        const int limit=std::max(lh,(pages.back().elements.empty()?available:h)-reserve-32);
        const bool valid=image.width>0&&image.height>0&&image.width<=1072&&image.height<=1448;
        const double scale=valid?std::min({1.0,double(w)/image.width,double(limit)/image.height}):1.0;
        const int iw=valid?std::max(1,int(image.width*scale)):w;
        const int ih=valid?std::max(1,int(image.height*scale)):std::min(120,limit);
        room(ih+32+reserve);
        PageElement item;item.line.start=item.line.end=image.offset;
        item.rect={(w-iw)/2,y+16,iw,ih};item.image=image.index;item.occurrence=static_cast<int>(n);
        add(std::move(item));y+=ih+32;
        text_range(image.offset,image.caption_end,true);
        cursor=image.caption_end;
    }
    text_range(cursor,value.size(),false);
    return pages;
}
std::size_t page_at(const std::vector<Page>& pages,std::size_t offset){
    if(pages.empty())return 0;
    for(std::size_t i=0;i<pages.size();++i)if(offset<pages[i].end)return i;
    return pages.size()-1;
}
std::string Canvas::ellipsize(const std::string& s,int size,int w){
    if(measure(s,size)<=w)return s;
    const auto suffix=std::string("…");int used=measure(suffix,size);std::size_t end=0;
    for(auto c:characters(s)){const int advance=glyph(c.cp,size).advance;if(used+advance>w)break;used+=advance;end=c.end;}
    return s.substr(0,end)+suffix;
}
void Canvas::write_pgm(const std::string& path)const{
    std::ofstream out(path,std::ios::binary);
    out<<"P5\n"<<width<<" "<<height<<"\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
    if(!out)throw std::runtime_error("无法写入预览图片");
}
}
