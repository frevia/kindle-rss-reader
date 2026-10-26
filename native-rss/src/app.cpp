#include "app.hpp"
#include "pinyin.hpp"
#include "feed.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <set>
#include <tuple>

namespace rss {
namespace {
std::int64_t midnight() {
    auto now=std::time(nullptr);auto local=*std::localtime(&now);
    local.tm_hour=local.tm_min=local.tm_sec=0;local.tm_isdst=-1;
    return std::mktime(&local);
}
std::string date(std::int64_t t) {
    if(t<=0)return "";
    auto time=static_cast<time_t>(t);auto local=std::localtime(&time);char out[40]{};
    if(local)std::strftime(out,sizeof(out),"%m.%d",local);
    return out;
}
bool under(const std::string& value,const std::string& parent){return value==parent||(!parent.empty()&&value.rfind(parent+"/",0)==0);}
std::string group_label(const std::string& path){return path.empty()?"未分组":path;}
std::string leaf(const std::string& path){auto slash=path.rfind('/');return slash==std::string::npos?path:path.substr(slash+1);}
std::string trim(const std::string& value){auto a=value.find_first_not_of(" \t\r\n"),b=value.find_last_not_of(" \t\r\n");return a==std::string::npos?"":value.substr(a,b-a+1);}
std::string lower_ascii(std::string value){for(auto& c:value)if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return value;}
void down(Canvas& c,int x,int y){for(int i=0;i<9;++i){c.fill({x+i,y+i,2,2},30);c.fill({x+16-i,y+i,2,2},30);}}
void frame(Canvas& c,Rect r,int weight=1){c.fill(r,30);c.fill({r.x+weight,r.y+weight,r.w-2*weight,r.h-2*weight},255);}
struct NavigationRow { std::string path; std::int64_t feed=0; int depth=0,height=104; };
std::vector<std::size_t> navigation_pages(const std::vector<NavigationRow>& rows,int available){
    std::vector<std::size_t> starts{0};int used=0;
    for(std::size_t i=0;i<rows.size();++i){int needed=rows[i].height;
        if(!rows[i].feed)for(std::size_t j=i+1;j<rows.size()&&under(rows[j].path,rows[i].path);++j){needed+=rows[j].height;if(rows[j].feed)break;}
        if(i>starts.back()&&used+std::min(needed,available)>available){starts.push_back(i);used=0;}
        used+=rows[i].height;
    }
    return starts;
}
int read_int(Store& store,const std::string& key,int fallback,int low,int high){
    try{return std::clamp(std::stoi(store.setting(key)),low,high);}catch(...){return fallback;}
}
}
App::App(Store& store,Canvas& canvas):store_(store),c_(canvas){
    font_size_=read_int(store_,"font_size",34,24,48);
    line_gap_=read_int(store_,"line_gap",24,10,42);
    margin_=read_int(store_,"margin",32,32,100);
    reload();
}
int App::row_height(const Entry& e){
    const int width=c_.width-108,meta=static_cast<int>(std::min<std::size_t>(2,c_.wrap(article_metadata(e),24,width).size()));
    const int cap=std::max(1,(list_bottom()-list_top()-meta*36-58)/50);
    return std::max(176,static_cast<int>(std::min<std::size_t>(cap,c_.wrap(e.title,36,width).size()))*50+meta*36+58);
}
void App::ensure_list_page(std::size_t page){
    while(!list_complete_&&list_starts_.size()<=page){
        const auto start=list_starts_.back();int y=list_top();std::size_t i=start;
        for(;i<list_.size();++i){const int height=row_height(list_[i]);if(y+height>list_bottom()&&i>start)break;y+=height;}
        if(i==list_.size())list_complete_=true;else list_starts_.push_back(i);
    }
}
// shortcut: list metadata still loads in full; use database paging if very large libraries make queries slow.
void App::reload(){
    feeds_=store_.feeds();group_names_=store_.groups();list_=store_.entries(view_,feed_,search_,midnight(),group_);
    list_starts_={0};list_complete_=false;
    ensure_list_page(list_page_);
    list_page_=std::min(list_page_,list_starts_.size()-1);dirty=true;
}
void App::show_view(View v,std::int64_t feed){
    view_=v;feed_=feed;group_.reset();search_.clear();list_page_=0;screen_=Screen::List;source_list_=false;reload();
}
void App::show_group(const std::string& group){
    view_=View::All;feed_=0;group_=group;search_.clear();list_page_=0;screen_=Screen::List;source_list_=false;reload();
}
std::string App::source(std::int64_t id)const{
    for(const auto& f:feeds_)if(f.id==id)return f.title.empty()?f.url:f.title;
    return "已保留的收藏";
}
std::string App::source_group(std::int64_t id)const{for(const auto& f:feeds_)if(f.id==id)return group_label(f.group);return "已取消订阅";}
std::string App::article_metadata(const Entry& e)const{return source(e.feed_id)+"  ·  "+source_group(e.feed_id)+"  ·  "+date(e.published);}
std::string App::view_name()const{
    if(!search_.empty())return "搜索结果";
    if(feed_)return source(feed_);
    if(group_)return group_->empty()?"未分组":*group_;
    switch(view_){case View::Unread:return "未读文章";case View::All:return "全部文章";
        case View::Starred:return "我的收藏";case View::Today:return "今日更新";}
    return "文章";
}
void App::button(Rect r,const std::string& label,std::function<void()> action,bool filled,int size,Icon icon,bool enabled,Align align){
    if(filled)c_.fill(r,25);
    const int symbol=icon==Icon::None?0:28,gap=symbol?12:0;
    const auto text=c_.ellipsize(label,size,r.w-20-symbol-gap);
    const int content=c_.measure(text,size)+symbol+gap;
    const int left=align==Align::Left?r.x:align==Align::Right?r.x+r.w-content:r.x+(r.w-content)/2;
    const auto color=filled?255:enabled?30:130;
    const bool trailing=icon==Icon::Next;
    if(symbol)c_.icon(icon,{trailing?left+c_.measure(text,size)+gap:left,r.y+(r.h-symbol)/2,symbol,symbol},color);
    c_.text(trailing?left:left+symbol+gap,r.y+(r.h+size)/2-5,text,size,color);
    if(enabled)hits_.push_back({r,std::move(action)});
}
void App::draw(){
    hits_.clear();c_.clear();
    if(!error_.empty()){
        c_.text(32,100,"操作未完成",40);int y=220;
        for(const auto& line:c_.wrap(error_,28,c_.width-64)){if(y>c_.height-220)break;c_.text(32,y,line.text,28);y+=44;}
        button({32,c_.height-100,c_.width-64,96},"返回",[this]{error_.clear();},true);
    }else if(!confirmation_.empty())confirmation_screen();
    else if(!input_mode_.empty())input_screen();
    else if(screen_==Screen::GroupPicker){if(picker_purpose_==PickerPurpose::Read)list_screen();group_picker_screen();}
    else if(managing_)management_screen();
    else if(screen_==Screen::GroupEdit)group_edit_screen();
    else if(current_){reader_screen();if(reader_tools_)reader_tools();if(reader_panel_!=ReaderPanel::None)reader_panel_screen();}
    else if(screen_==Screen::Subscriptions||screen_==Screen::Groups)nav_screen();
    else if(screen_==Screen::Opml)opml_screen();
    else if(screen_==Screen::Sync)sync_screen();
    else if(screen_==Screen::Cache)management_screen();
    else {list_screen();if(brand_menu_)app_menu();}
    dirty=false;
}
void App::list_screen(){
    ensure_list_page(list_page_+1);const int w=c_.width,h=c_.height,m=32;
    c_.text(32,76,"RSS Reader",36,30);down(c_,32+c_.measure("RSS Reader",36)+20,58);
    hits_.push_back({{32,8,310,88},[this]{brand_menu_=true;}});
    c_.icon(Icon::Sync,{w-66,43,34,34});
    hits_.push_back({{w-120,8,88,88},[this]{screen_=Screen::Sync;if(sync_state_!=SyncState::Running){request_="sync";sync_started();}}});
    c_.rule(32,104,w-64,30);
    const auto total=std::to_string(list_.size());const int total_width=c_.measure(total,48),extra=search_.empty()?0:200;
    c_.text(m,174,c_.ellipsize(view_name(),52,w-2*m-total_width-40-extra),52);
    c_.text(w-m-total_width-extra,174,total,48,75);
    if(!search_.empty())button({w-244,118,180,88},"退出搜索",[this]{clear_search();},false,26);
    const auto context=!search_.empty()?"「"+search_+"」 · "+(feed_?source(feed_):group_?group_label(*group_):"全部订阅"):
        feed_?source_group(feed_):group_?"订阅 / "+group_label(*group_):"";
    if(!context.empty())c_.text(m,214,c_.ellipsize(context,24,w-64),24,85);
    const int filters=list_top()-88;
    c_.icon(Icon::Folder,{m,filters+34,28,28});
    const auto scope=feed_?"切换分组":group_?group_label(leaf(*group_)):"全部分组";
    c_.text(m+40,filters+58,c_.ellipsize(scope,26,148),26);down(c_,m+204,filters+43);
    hits_.push_back({{m,filters,232,88},[this]{open_group_picker(PickerPurpose::Read);}});
    c_.fill({m+244,filters,1,88},180);
    const std::vector<std::pair<std::string,View>> choices={{"未读",View::Unread},{"全部",View::All},{"收藏",View::Starred}};
    for(std::size_t i=0;i<choices.size();++i){int x=m+264+static_cast<int>(i)*116;bool selected=view_==choices[i].second;
        c_.text(x,filters+58,choices[i].first,28,selected?20:90);if(selected)c_.fill({x,filters+84,88,4},25);
        hits_.push_back({{x,filters,100,88},[this,v=choices[i].second]{view_=v;list_page_=0;reload();}});
    }
    c_.icon(Icon::Search,{w-124,filters+34,28,28});c_.text(w-32-c_.measure("搜索",26),filters+58,"搜索",26,30);
    hits_.push_back({{w-168,filters,136,88},[this]{start_input("search",search_);}});c_.rule(m,list_top(),w-64,100);
    if(list_.empty()){
        const bool none=feeds_.empty()&&search_.empty(),empty_search=!search_.empty(),empty_star=view_==View::Starred;
        const bool done=view_==View::Unread&&!store_.entries(View::All,feed_,"",midnight(),group_).empty();
        const auto title=none?"从第一份订阅开始":empty_search?"没有找到相关标题":empty_star?"还没有收藏":done?"未读已全部读完":"这里还没有文章";
        const std::string copy=none?"添加 RSS 地址，或导入 OPML 文件。内容缓存后，即可离线阅读。":
            empty_search?"没有标题包含「"+search_+"」。试试更短的关键词，或切换搜索范围。":
            empty_star?"阅读时点击顶部星标，即可收藏文章。":done?"慢慢读，也可以就此休息。已读文章仍可在「全部」中找到。":"同步后，已缓存的文章会显示在这里。";
        const std::string label=none?"添加订阅":empty_search?"修改搜索":done?"查看全部文章":"订阅";
        auto lines=c_.wrap(copy,30,w-64);
        const int fixed_height=160+40+67+24+48+96;
        const int available=list_bottom()-list_top(),max_lines=std::max(1,(available-150-fixed_height)/54);
        if(lines.size()>static_cast<std::size_t>(max_lines)){lines.resize(max_lines);lines.back().text=c_.ellipsize(lines.back().text+"…",30,w-64);}
        const int content_height=fixed_height+static_cast<int>(lines.size())*54;
        int y=list_top()+110+std::max(0,(available-150-content_height)/2);
        c_.fill({32,y,110,3},20);c_.rule(32,y+159,110,20);
        if(none||done)c_.text(32,y+116,none?"＋":"✓",84);else c_.fill({32,y+84,84,4},20);
        y+=160+40;c_.text(32,y+52,title,48);y+=67+24;
        for(const auto& line:lines){c_.text(32,y+38,line.text,30,90);y+=54;}
        y+=48;const Rect action{32,y,std::max(160,c_.measure(label,28)+56),96};frame(c_,action,2);
        button(action,label,[this,none,empty_search,done]{
            if(none)edit_feed(Feed{});
            else if(empty_search)start_input("search",search_);
            else if(done){view_=View::All;list_page_=0;reload();}
            else open_subscriptions();
        },false,28);
    }
    int y=list_top();const auto end=list_page_+1<list_starts_.size()?list_starts_[list_page_+1]:list_.size();
    for(auto i=list_starts_[list_page_];i<end;++i){auto& e=list_[i];const int height=row_height(e);
        if(auto fresh=store_.entry(e.id,false)){e.read=fresh->read;e.starred=fresh->starred;}
        auto meta=c_.wrap(article_metadata(e),24,w-108);const int meta_count=static_cast<int>(std::min<std::size_t>(2,meta.size()));
        auto title=c_.wrap(e.title,36,w-108);const int count=std::min<int>(title.size(),std::max(1,(height-meta_count*36-58)/50));
        if(e.starred)c_.icon(Icon::StarFilled,{m+1,y+36,26,26});else if(!e.read)c_.text(m+7,y+57,"•",24,20);
        for(int n=0;n<count;++n){auto text=title[n].text;if(n==count-1&&count<static_cast<int>(title.size()))text=c_.ellipsize(text+"…",36,w-108);c_.text(m+44,y+60+n*50,text,36,e.read?65:20);}
        for(int n=0;n<meta_count;++n){auto text=meta[n].text;if(n==meta_count-1&&meta.size()>2)text=c_.ellipsize(text+"…",24,w-108);c_.text(m+44,y+24+count*50+10+24+n*36,text,24,85);}
        c_.rule(m,y+height-1,w-64,205);hits_.push_back({{m,y,w-64,height},[this,id=e.id]{open(id);}});y+=height;
    }
    if(!status_.empty())button({m,h-204,w-64,88},status_,[this]{status_.clear();reload();},false,22);
    c_.rule(32,h-104,w-64,210);
    button({32,h-100,220,96},"上一页",[this]{previous();},false,26,Icon::Back,list_page_>0,Align::Left);
    const auto page="第 "+std::to_string(list_page_+1)+" / "+(list_complete_?std::to_string(list_starts_.size()):"…")+" 页";
    c_.text(w/2-c_.measure(page,24)/2,h-42,page,24,90);
    button({w-252,h-100,220,96},"下一页",[this]{next();},false,26,Icon::Next,list_page_+1<list_starts_.size(),Align::Right);
}
int App::article_heading_height(){
    return 60*static_cast<int>(std::min<std::size_t>(3,c_.wrap(current_->title,48,c_.width-2*margin_).size()))+76;
}
void App::paginate(){
    if(!current_)return;
    std::vector<InlineImage> inline_images;
    if(!current_->html.empty()&&html_to_text(current_->html)==current_->text){
        std::string base=current_->url;
        if(base.empty())for(const auto& feed:feeds_)if(feed.id==current_->feed_id){base=feed.url;break;}
        for(const auto& anchor:html_image_anchors(current_->html,base)){
            auto found=std::find_if(current_->images.begin(),current_->images.end(),[&](const ArticleImage& image){return image.url==anchor.url;});
            if(found==current_->images.end())continue;
            inline_images.push_back({anchor.offset,anchor.caption_end,static_cast<int>(found-current_->images.begin()),found->width,found->height});
        }
    }
    pages_=c_.paginate_article(current_->text,inline_images,font_size_,line_height(),c_.width-2*margin_,c_.height-252,
                               c_.height-252-article_heading_height());
    // Existing reading positions count the title prefix; images consume no text bytes.
    const auto prefix=current_->title.size()+2;
    for(auto& page:pages_){page.start+=prefix;page.end+=prefix;}
    pages_.front().start=0;
    const auto position=static_cast<std::size_t>(std::max<std::int64_t>(0,current_->position));
    page_=page_at(pages_,position);
    const auto anchor=store_.setting("reader_image:"+std::to_string(current_->id));
    for(std::size_t i=0;i<pages_.size();++i)
        if(pages_[i].image_anchor>=0&&anchor==std::to_string(position)+":"+std::to_string(pages_[i].image_anchor)){page_=i;break;}

}
void App::open(std::int64_t id,bool track){
    auto entry=store_.entry(id);if(!entry)throw std::runtime_error("文章已不在缓存中");
    if(entry->text.empty()&&entry->images.empty())throw std::runtime_error("这篇文章尚无可阅读的正文");
    current_=std::move(entry);paginate();
    if(track){
        store_.set_read(id,true);current_->read=true;
        store_.set_position(id,current_->position);
        store_.set_setting("last_entry",std::to_string(id));
    }
    reader_tools_=false;reader_panel_=ReaderPanel::None;image_zoom_=false;image_page_=0;info_scroll_=0;typography_anchor_.reset();dirty=true;
}
void App::save_position(){
    if(!current_||pages_.empty())return;
    current_->position=static_cast<std::int64_t>(pages_[page_].start);
    store_.set_position(current_->id,current_->position);
    // Consecutive image-only pages share a text offset; retain their occurrence separately.
    store_.set_setting("reader_image:"+std::to_string(current_->id),
                       std::to_string(current_->position)+":"+std::to_string(pages_[page_].image_anchor));
}
void App::reader_screen(){
    const int w=c_.width,h=c_.height;
    button({32,8,230,88},"返回列表",[this]{save_position();current_.reset();pages_.clear();reader_tools_=false;reader_panel_=ReaderPanel::None;},false,26,Icon::Back,true,Align::Left);
    button({w-262,8,230,88},current_->starred?"已收藏":"未收藏",[this]{
        current_->starred=!current_->starred;store_.set_starred(current_->id,current_->starred);
    },false,26,current_->starred?Icon::StarFilled:Icon::Star,true,Align::Right);
    c_.rule(32,108,w-64);
    const int heading=page_==0?article_heading_height():0;
    const int top=148+heading;
    if(page_==0){
        const auto title=c_.wrap(current_->title,48,w-2*margin_);
        for(std::size_t i=0;i<std::min<std::size_t>(3,title.size());++i)
            c_.text(margin_,174+static_cast<int>(i)*60,i==2&&title.size()>3?c_.ellipsize(title[i].text+"…",48,w-2*margin_):title[i].text,48,15);
        const auto meta=source(current_->feed_id)+"  ·  "+date(current_->published);
        c_.text(margin_,174+static_cast<int>(std::min<std::size_t>(3,title.size()))*60+8,c_.ellipsize(meta,24,w-2*margin_),24,75);
    }else{
        c_.text(margin_,135,c_.ellipsize(current_->title,24,w-2*margin_),24,90);
    }
    const int panel_height=reader_panel_==ReaderPanel::Type?420:reader_panel_==ReaderPanel::Images?(image_zoom_?764:662):reader_panel_==ReaderPanel::Info?520:0;
    const int cover_top=reader_panel_==ReaderPanel::None?(reader_tools_?h-120:h-104):h-120-panel_height;
    for(const auto& item:pages_[page_].elements){
        const Rect area{margin_+item.rect.x,top+item.rect.y,item.rect.w,item.rect.h};
        if(area.y>=cover_top||(item.image<0&&area.y+area.h>cover_top))continue;
        if(item.image>=0){
            auto cached=store_.image(current_->id,current_->images.at(item.image).url);
            if(!cached||!draw_image(*cached,area,false,cover_top)){
                c_.fill({area.x,area.y,area.w,std::min(area.h,cover_top-area.y)},245);
                if(area.y+area.h/2+10<=cover_top)c_.text(area.x+20,area.y+area.h/2+10,c_.ellipsize("图片未缓存 · 点击查看",24,area.w-40),24,95);
            }
        }else c_.text(area.x+(item.caption?(area.w-c_.measure(item.line.text,item.size))/2:0),
                      area.y+item.size,item.line.text,item.size,item.caption?90:20);
    }
    if(reader_panel_==ReaderPanel::None&&!reader_tools_){
        c_.rule(32,h-104,w-64,200);
        c_.fill({32,h-105,static_cast<int>((w-64)*(page_+1)/pages_.size()),3},30);
        const auto footer="第 "+std::to_string(page_+1)+" / "+std::to_string(pages_.size())+" 页";
        button({32,h-100,230,96},"上一页",[this]{previous();},false,26,Icon::Back,page_>0,Align::Left);
        c_.text((w-c_.measure(footer,24))/2,h-35,footer,24,100);
        if(page_+1<pages_.size())button({w-262,h-100,230,96},"下一页",[this]{next();},false,26,Icon::Next,true,Align::Right);
        else button({w-262,h-100,230,96},"下一篇",[this]{
            store_.finish_reading(current_->id);
            auto it=std::find_if(list_.begin(),list_.end(),[this](const Entry& e){return e.id==current_->id;});
            if(it!=list_.end()&&++it!=list_.end())open(it->id);
            else{current_.reset();pages_.clear();notify("本次列表已读完");}
        },false,26,Icon::Next,true,Align::Right);
    }
    const int content_bottom=reader_panel_==ReaderPanel::None?(reader_tools_?h-120:h-104):cover_top;
    hits_.push_back({{0,110,w,content_bottom-110},[this,w]{
        if(reader_panel_!=ReaderPanel::None){close_reader_tools();return;}
        const int x=last_tap_x_;
        if(reader_tools_){close_reader_tools();return;}
        if(x<w/4)previous();else if(x>=3*w/4)next();else{reader_tools_=true;}
    }});
    if(reader_panel_==ReaderPanel::None&&!reader_tools_){
        for(const auto& item:pages_[page_].elements)if(item.image>=0){
            const Rect area{margin_+item.rect.x,top+item.rect.y,item.rect.w,item.rect.h};
            if(area.y+area.h<=content_bottom)hits_.push_back({area,[this,index=item.image,w]{
                image_page_=index;
                if(last_tap_x_>=w/4&&last_tap_x_<3*w/4){reader_tools_=true;return;}
                image_page_=index;reader_tools_=true;reader_panel_=ReaderPanel::Images;image_zoom_=false;
            }});
        }
    }
}
void App::close_reader_tools(){reader_panel_=ReaderPanel::None;reader_tools_=false;image_zoom_=false;typography_anchor_.reset();}
void App::reader_tools(){
    const int w=c_.width,h=c_.height,y=h-120;
    c_.fill({0,y,w,120},255);c_.rule(32,y,w-64,170);
    const std::array<std::tuple<const char*,Icon,ReaderPanel>,3> tools{{
        {"排版",Icon::None,ReaderPanel::Type},{"图片",Icon::Image,ReaderPanel::Images},{"信息",Icon::Info,ReaderPanel::Info}}};
    for(std::size_t i=0;i<tools.size();++i){
        const int tw=(w-64)/3,x=32+static_cast<int>(i)*tw;
        const auto label=std::get<0>(tools[i]);const auto icon=std::get<1>(tools[i]);const auto panel=std::get<2>(tools[i]);
        if(icon==Icon::None)c_.text(x+(tw-c_.measure("Aa",34))/2,y+51,"Aa",34);
        else c_.icon(icon,{x+(tw-34)/2,y+20,34,34});
        if(reader_panel_==panel)c_.fill({x,y+116,tw,4},30);
        c_.text(x+(tw-c_.measure(label,24))/2,y+88,label,24);
        hits_.push_back({{x,y,tw,120},[this,panel]{
            typography_anchor_.reset();
            if(reader_panel_==panel){reader_panel_=ReaderPanel::None;image_zoom_=false;}
            else{reader_panel_=panel;image_zoom_=false;info_scroll_=0;}
        }});
    }
}
void App::reader_panel_screen(){
    const int w=c_.width,h=c_.height,bar=120;
    const int height=reader_panel_==ReaderPanel::Type?420:reader_panel_==ReaderPanel::Images?(image_zoom_?764:662):520;
    const int y=h-bar-height;
    c_.fill({0,y,w,height},255);c_.rule(32,y,w-64,150);
    const auto close=[this]{reader_panel_=ReaderPanel::None;image_zoom_=false;typography_anchor_.reset();};
    const char* title=reader_panel_==ReaderPanel::Type?"排版":reader_panel_==ReaderPanel::Images?"图片":"文章信息";
    c_.text(32,y+54,title,28);
    button({w-264,y,232,88},"收起面板",close,false,24,Icon::Close,true,Align::Right);
    auto adjust=[this](int& value,int next,const std::string& key){
        if(value==next)return;
        // Keep one text/image anchor across repeated live adjustments instead of drifting to each new page start.
        if(!typography_anchor_){save_position();typography_anchor_=current_->position;}
        current_->position=*typography_anchor_;
        value=next;store_.set_setting(key,std::to_string(value));paginate();
    };
    if(reader_panel_==ReaderPanel::Type){
        const int start=w-496,cell=144,gap=16;
        const auto frame=[this](Rect r,bool selected=false){
            const int edge=selected?3:1;c_.fill(r,selected?30:180);
            c_.fill({r.x+edge,r.y+edge,r.w-2*edge,r.h-2*edge},255);
        };
        c_.text(32,y+152,"字号",26);
        const Rect smaller{start,y+96,cell,88},larger{start+2*(cell+gap),y+96,cell,88};
        frame(smaller);frame(larger);
        button(smaller,"−",[this,adjust]{adjust(font_size_,std::max(24,font_size_-2),"font_size");},false,34,Icon::None,font_size_>24);
        const auto value=std::to_string(font_size_);
        c_.text(start+cell+gap+(cell-c_.measure(value,30))/2,y+152,value,30);
        button(larger,"+",[this,adjust]{adjust(font_size_,std::min(48,font_size_+2),"font_size");},false,34,Icon::None,font_size_<48);
        const std::array<int,3> gaps{16,24,32};
        const std::array<int,4> margins{32,64,78,96};
        const std::array<const char*,3> lines{"紧凑","标准","宽松"};
        const std::array<const char*,4> widths{"窄","适中","标准","宽"};
        c_.text(32,y+252,"行距  "+std::to_string(line_height()),26);
        c_.text(32,y+352,"页边距  "+std::to_string(margin_),26);
        for(int i=0;i<3;++i){
            const Rect line{start+i*(cell+gap),y+196,cell,88};
            frame(line,line_gap_==gaps[i]);
            button(line,lines[i],[this,adjust,gaps,i]{adjust(line_gap_,gaps[i],"line_gap");},false,26);
        }
        for(int i=0;i<4;++i){
            const Rect margin{start+i*(104+gap),y+296,104,88};frame(margin,margin_==margins[i]);
            button(margin,widths[i],[this,adjust,margins,i]{adjust(margin_,margins[i],"margin");},false,26);
        }
    }else if(reader_panel_==ReaderPanel::Images){
        if(current_->images.empty())c_.text(32,y+240,"这篇文章没有图片",28,90);
        else{
            const int area_height=image_zoom_?542:440,area_x=image_zoom_?16:32,area_w=w-2*area_x;
            const Rect area{area_x,y+88,area_w,area_height};const auto& reference=current_->images.at(image_page_);
            auto cached=store_.image(current_->id,reference.url);
            const bool shown=cached&&cached->error.empty()&&draw_image(*cached,area,true);
            if(shown)hits_.push_back({area,[this]{image_zoom_=!image_zoom_;}});
            else{
                const auto message=cached&&!cached->error.empty()?"图片加载失败："+cached->error:"图片尚未缓存，返回列表同步后可离线查看。";
                int baseline=area.y+70;
                for(const auto& line:c_.wrap(message,24,area.w)){
                    if(baseline>area.y+area.h-16)break;
                    c_.text(area.x,baseline,line.text,24,90);baseline+=38;
                }
            }
            const auto caption=reference.alt.empty()?"订阅源图片":reference.alt;
            c_.text(32,y+area_height+118,c_.ellipsize(caption,22,w-64),22,90);
            const int controls=h-bar-100;
            button({32,controls,216,88},"上一张",[this]{previous();},false,24,Icon::Back,image_page_>0,Align::Left);
            const auto counter=std::to_string(image_page_+1)+" / "+std::to_string(current_->images.size());
            c_.text(w/2-180+(160-c_.measure(counter,24))/2,controls+52,counter,24,90);
            button({w/2+12,controls,176,88},image_zoom_?"还原":"放大",[this]{image_zoom_=!image_zoom_;},false,24,Icon::None,shown);
            button({w-248,controls,216,88},"下一张",[this]{next();},false,24,Icon::Next,image_page_+1<current_->images.size(),Align::Right);
        }
    }else{
        const std::string details="标题："+current_->title+"\n\n来源："+source(current_->feed_id)+"\n时间："+date(current_->published)+"\n作者："+(current_->author.empty()?"未提供":current_->author)+"\n\n原文地址：\n"+current_->url;
        const auto lines=c_.wrap(details,24,w-64);const std::size_t visible=std::max(1,(height-210)/38);
        const auto last=((lines.size()-1)/visible)*visible;info_scroll_=std::min(info_scroll_,last);
        for(std::size_t i=0;i<visible&&info_scroll_+i<lines.size();++i)c_.text(32,y+128+static_cast<int>(i)*38,lines[info_scroll_+i].text,24,30);
        if(lines.size()>visible){
            button({32,y+height-100,230,88},"上一页",[this,visible]{info_scroll_=info_scroll_>=visible?info_scroll_-visible:0;},false,24,Icon::Back,info_scroll_>0,Align::Left);
            button({w-262,y+height-100,230,88},"下一页",[this,visible,last]{info_scroll_=std::min(info_scroll_+visible,last);},false,24,Icon::Next,info_scroll_+visible<lines.size(),Align::Right);
        }
        button({w/2-210,y+height-100,420,88},current_->read?"标为未读":"标为已读",[this]{current_->read=!current_->read;store_.set_read(current_->id,current_->read);},false,24,Icon::Check);
    }
}
void App::open_subscriptions(){
    home_=ListContext{view_,feed_,group_,search_,list_page_};status_.clear();source_list_=false;brand_menu_=false;screen_=Screen::Subscriptions;nav_page_=0;
    feeds_=store_.feeds();group_names_=store_.groups();
}
void App::restore_home(){
    if(home_){view_=home_->view;feed_=home_->feed;group_=home_->group;search_=home_->search;list_page_=home_->page;}
    source_list_=false;screen_=Screen::List;reload();
}
void App::app_menu(){
    hits_.clear();hits_.push_back({{0,0,c_.width,c_.height},[this]{brand_menu_=false;}});
    frame(c_,{32,104,400,212},2);
    button({56,108,352,100},"订阅",[this]{open_subscriptions();},false,30,Icon::Folder,true,Align::Left);
    c_.rule(34,210,396,180);
    button({56,212,352,100},"退出",[this]{brand_menu_=false;confirmation_="exit";},false,30,Icon::Exit,true,Align::Left);
}
void App::nav_screen(){
    const bool groups=screen_==Screen::Groups;const int w=c_.width,h=c_.height;
    button({32,8,240,88},groups?"订阅":"文章列表",[this]{back();},false,26,Icon::Back,true,Align::Left);
    button({w-212,8,180,88},groups?"新增":"添加",[this,groups]{if(groups){edit_group("",true);}else edit_feed(Feed{});},false,28,Icon::Plus,true,Align::Right);
    c_.rule(32,104,w-64,30);c_.text(32,174,groups?"管理分组":"订阅",48);
    c_.text(32,214,std::to_string(groups?group_names_.size()-1:feeds_.size())+(groups?" 个分组":" 个订阅源"),24,85);
    const auto count=[this](const std::string& path){return std::count_if(feeds_.begin(),feeds_.end(),[&](const Feed& f){return path.empty()?f.group.empty():under(f.group,path);});};
    std::vector<NavigationRow> rows;
    if(groups){for(const auto& path:group_names_)rows.push_back({path,0,static_cast<int>(std::count(path.begin(),path.end(),'/')),128});}
    else{
        std::function<void(const std::string&)> add=[&](const std::string& path){
            const int depth=static_cast<int>(std::count(path.begin(),path.end(),'/'));rows.push_back({path,0,depth,104});
            if(folded_.count(path))return;
            for(const auto& f:feeds_)if(f.group==path)rows.push_back({path,f.id,depth+1,128});
            for(const auto& child:group_names_){auto slash=child.rfind('/');if(slash!=std::string::npos&&child.substr(0,slash)==path)add(child);}
        };
        if(count("")>0)add("");for(const auto& path:group_names_)if(!path.empty()&&path.find('/')==std::string::npos)add(path);
    }
    const int top=250,bottom=h-(groups?104:208);auto starts=navigation_pages(rows,bottom-top);auto& page=groups?groups_page_:nav_page_;
    page=std::min(page,starts.size()-1);nav_page_count_=starts.size();int y=top;const auto end=page+1<starts.size()?starts[page+1]:rows.size();
    if(starts[page]<rows.size()&&rows[starts[page]].depth>0)c_.text(32,244,c_.ellipsize(rows[starts[page]].path,22,w-64),22,100);
    for(auto i=starts[page];i<end;++i){const auto row=rows[i];const int x=40+std::min(row.depth,8)*32;
        if(!row.feed){
            c_.icon(Icon::Folder,{x,y+34,28,28});c_.text(x+48,y+62,c_.ellipsize(group_label(leaf(row.path)),30,w-x-(groups?450:310)),30);
            if(groups){
                c_.text(x+48,y+100,std::to_string(count(row.path))+" 个订阅源",24,90);
                if(!row.path.empty()){
                    button({w-352,y+10,176,88},"子分组",[this,path=row.path]{edit_group(path,true);},false,26,Icon::Plus,true,Align::Right);
                    button({w-152,y+10,120,88},"编辑",[this,path=row.path]{edit_group(path,false);},false,26,Icon::None,true,Align::Right);
                }
            }else{
                const auto label=std::to_string(count(row.path))+" 个源";c_.text(w-72-c_.measure(label,24),y+62,label,24,90);down(c_,w-52,y+44);
                hits_.push_back({{32,y,w-64,row.height},[this,path=row.path]{if(folded_.count(path))folded_.erase(path);else folded_.insert(path);}});
            }
        }else{
            auto f=std::find_if(feeds_.begin(),feeds_.end(),[&](const Feed& f){return f.id==row.feed;});if(f==feeds_.end())continue;
            c_.icon(Icon::Rss,{x,y+36,28,28});auto title=c_.wrap(f->title.empty()?f->url:f->title,32,w-x-240);int n=0;
            for(const auto& line:title){if(n==2)break;auto text=line.text;if(n==1&&title.size()>2)text=c_.ellipsize(text+"…",32,w-x-240);c_.text(x+48,y+44+n*38,text,32);++n;}
            auto domain=f->url;auto protocol=domain.find("://");if(protocol!=std::string::npos)domain=domain.substr(protocol+3);domain=domain.substr(0,domain.find('/'));
            c_.text(x+48,y+36+n*38,c_.ellipsize(domain,24,w-x-240),24,90);
            hits_.push_back({{32,y,w-224,row.height},[this,id=f->id]{view_=View::Unread;feed_=id;group_.reset();search_.clear();list_page_=0;source_list_=true;screen_=Screen::List;reload();}});
            button({w-152,y+20,120,88},"编辑",[this,f=*f]{edit_feed(f);},false,26,Icon::None,true,Align::Right);
        }
        c_.rule(32,y+row.height-1,w-64,205);y+=row.height;
    }
    if(rows.empty())c_.text(32,350,"还没有订阅源",32,80);
    const int pager=h-(groups?100:204);
    button({32,pager,220,88},"上一页",[page_ptr=&page]{--*page_ptr;},false,26,Icon::Back,page>0,Align::Left);
    const auto page_label="第 "+std::to_string(page+1)+" / "+std::to_string(starts.size())+" 页";c_.text(w/2-c_.measure(page_label,24)/2,pager+56,page_label,24,90);
    button({w-252,pager,220,88},"下一页",[page_ptr=&page]{++*page_ptr;},false,26,Icon::Next,page+1<starts.size(),Align::Right);
    if(!groups){c_.rule(32,h-104,w-64,100);button({32,h-100,w/2-64,88},"管理分组",[this]{screen_=Screen::Groups;},false,28,Icon::Folder,true,Align::Left);button({w/2,h-100,w/2-32,88},"OPML",[this]{status_.clear();screen_=Screen::Opml;},false,28,Icon::Download,true,Align::Right);}
}
void App::open_group_picker(PickerPurpose purpose){
    picker_purpose_=purpose;picker_query_.clear();picker_page_=0;
    if(!input_mode_.empty()){suspended_input_=InputContext{input_mode_,input_,composing_,chinese_,symbols_,shift_,candidate_page_};input_mode_.clear();}
    screen_=Screen::GroupPicker;
}
void App::restore_input(){
    if(suspended_input_){auto old=*suspended_input_;input_mode_=old.mode;input_=old.value;composing_=old.composing;chinese_=old.chinese;symbols_=old.symbols;shift_=old.shift;candidate_page_=old.candidate;suspended_input_.reset();}
    screen_=picker_purpose_==PickerPurpose::Move?edit_origin_:picker_purpose_==PickerPurpose::Parent?Screen::GroupEdit:Screen::List;
}
void App::group_picker_screen(){
    const int w=c_.width,h=c_.height;const bool popup=picker_purpose_==PickerPurpose::Read,parent=picker_purpose_==PickerPurpose::Parent;
    const int left=32,width=w-64,row_height=popup?88:112;
    int top=popup?list_top()+96:250;
    if(!popup){
        button({32,8,220,88},"返回",[this]{back();},false,26,Icon::Back,true,Align::Left);
        button({w-212,8,180,88},"查找",[this]{start_input("group-find",picker_query_);},false,26,Icon::Search,true,Align::Right);
        c_.rule(32,104,w-64,30);c_.text(32,174,parent?"上级分组":picker_purpose_==PickerPurpose::Search?"搜索范围":"所属分组",48);
        c_.text(32,214,parent?"选择顶层，或放入已有分组；子分组随之移动":picker_purpose_==PickerPurpose::Move?"每个订阅源归属一个分组":"包含所选分组及其子分组",24,85);
        if(!picker_query_.empty()){c_.text(32,286,c_.ellipsize("查找："+picker_query_,26,w-260),26);button({w-212,238,180,88},"清除",[this]{picker_query_.clear();picker_page_=0;},false,26,Icon::None,true,Align::Right);top=340;}
    }
    std::vector<std::optional<std::string>> options;if(!parent&&picker_purpose_!=PickerPurpose::Move)options.push_back(std::nullopt);for(const auto& group:group_names_)if(!parent||new_group_||!under(group,editing_group_))options.push_back(group);
    std::vector<NavigationRow> rows;for(std::size_t i=0;i<options.size();++i){const auto name=options[i]?(parent&&options[i]->empty()?"顶层（无上级）":group_label(*options[i])):"全部订阅";if(lower_ascii(name).find(lower_ascii(picker_query_))!=std::string::npos)rows.push_back({name,static_cast<std::int64_t>(i+1),options[i]?static_cast<int>(std::count(options[i]->begin(),options[i]->end(),'/')):0,row_height});}
    auto starts=navigation_pages(rows,popup?6*row_height:h-104-top);picker_page_=std::min(picker_page_,starts.size()-1);nav_page_count_=starts.size();const auto begin=starts[picker_page_],end=picker_page_+1<starts.size()?starts[picker_page_+1]:rows.size();
    const int visible_rows=starts.size()>1?6:static_cast<int>(std::max<std::size_t>(1,end-begin));
    if(popup){
        const Rect panel{left,top-88,width,88+visible_rows*row_height+(starts.size()>1?88:0)+4};
        hits_.clear();hits_.push_back({{0,0,w,h},[this]{back();}});frame(c_,panel,2);hits_.push_back({panel,[]{}});
        if(picker_query_.empty())c_.text(left+24,panel.y+52,"选择分组",26);
        else button({left+24,panel.y,width-236,88},"查找："+picker_query_,[this]{start_input("group-find",picker_query_);},false,26,Icon::None,true,Align::Left);
        button({left+width-188,panel.y,164,88},picker_query_.empty()?"查找":"清除",[this]{if(picker_query_.empty())start_input("group-find",picker_query_);else{picker_query_.clear();picker_page_=0;}},false,26,picker_query_.empty()?Icon::Search:Icon::None,true,Align::Right);
        c_.rule(left+16,top-1,width-32,180);
    }
    int y=top;
    for(auto i=begin;i<end;++i){const auto row=rows[i];const auto option=options.at(row.feed-1);const int x=popup?left+24+std::min(row.depth,6)*28:40+std::min(row.depth,8)*32;
        c_.icon(Icon::Folder,{x,y+(popup?34:40),28,28});c_.text(x+48,y+(popup?58:68),c_.ellipsize(row.path,popup?28:30,left+width-x-(popup?144:112)),popup?28:30);
        const bool selected=parent?(option&&*option==group_parent_draft_):picker_purpose_==PickerPurpose::Move?(option&&*option==managing_->group):(!feed_&&option==group_);
        if(selected)c_.icon(Icon::Check,{left+width-(popup?56:28),y+(popup?34:40),28,28});c_.rule(popup?left+16:left,y+row_height-1,popup?width-32:width,205);
        hits_.push_back({{popup?left+2:left,y,popup?width-4:width,row_height},[this,option]{
            if(picker_purpose_==PickerPurpose::Parent){group_parent_draft_=option.value_or("");screen_=Screen::GroupEdit;}
            else if(picker_purpose_==PickerPurpose::Move){managing_->group=option.value_or("");screen_=edit_origin_;}
            else {group_=option;feed_=0;if(picker_purpose_==PickerPurpose::Search){restore_input();}else{source_list_=false;screen_=Screen::List;list_page_=0;reload();}}
        }});y+=row_height;
    }
    if(rows.empty())c_.text(popup?left+24:32,top+(popup?58:100),"没有匹配的分组",popup?28:32,80);
    if(!popup||starts.size()>1){
        const int pager=popup?top+visible_rows*row_height:h-100;
        button({popup?left+16:32,pager,popup?200:220,88},"上一页",[this]{--picker_page_;},false,26,Icon::Back,picker_page_>0,Align::Left);
        const auto page="第 "+std::to_string(picker_page_+1)+" / "+std::to_string(starts.size())+" 页";c_.text(left+width/2-c_.measure(page,24)/2,pager+56,page,24,90);
        button({popup?left+width-216:w-252,pager,popup?200:220,88},"下一页",[this]{++picker_page_;},false,26,Icon::Next,picker_page_+1<starts.size(),Align::Right);
    }
}
void App::edit_group(const std::string& path,bool create){
    new_group_=create;editing_group_=create?"":path;group_name_draft_=create?"":leaf(path);
    const auto slash=path.rfind('/');group_parent_draft_=create?path:slash==std::string::npos?"":path.substr(0,slash);
    screen_=Screen::GroupEdit;
}
void App::group_edit_screen(){
    const int w=c_.width,h=c_.height;button({32,8,220,88},"管理分组",[this]{screen_=Screen::Groups;},false,26,Icon::Back,true,Align::Left);c_.rule(32,104,w-64,30);
    c_.text(32,174,new_group_?"新增分组":"编辑分组",48);c_.text(32,270,"分组名称",24,85);frame(c_,{32,296,w-64,112});
    button({56,304,w-112,96},group_name_draft_.empty()?"点击输入名称":group_name_draft_,[this]{start_input("folder",group_name_draft_);},false,30,Icon::None,true,Align::Left);
    c_.text(32,470,"上级分组",24,85);frame(c_,{32,496,w-64,112});
    button({56,504,w-168,96},group_parent_draft_.empty()?"顶层（无上级）":group_parent_draft_,[this]{open_group_picker(PickerPurpose::Parent);},false,30,Icon::Folder,true,Align::Left);
    down(c_,w-72,552);hits_.push_back({{w-112,496,80,112},[this]{open_group_picker(PickerPurpose::Parent);}});
    c_.text(32,682,new_group_?"保存在所选上级中，可继续添加更深的子分组。":"更换上级会移动整个分组，订阅和文章状态保留。",26,85);
    if(!new_group_){button({32,780,280,88},"删除分组",[this]{confirmation_="delete-group";},false,28,Icon::Close,true,Align::Left);c_.text(32,936,"删除含子分组；订阅移到未分组，文章保留。",26,85);}
    button({32,h-100,220,88},"取消",[this]{screen_=Screen::Groups;},false,28,Icon::None,true,Align::Left);
    button({w-252,h-100,220,88},"保存",[this]{
        const auto name=trim(group_name_draft_);if(name.empty()||name.find('/')!=std::string::npos)throw std::runtime_error("请输入不含 / 的分组名称");
        const auto target=(group_parent_draft_.empty()?"":group_parent_draft_+"/")+name;
        if(new_group_)store_.add_group(target);else{
            store_.rename_group(editing_group_,target);
            const auto remap=[&](std::optional<std::string>& path){if(path&&under(*path,editing_group_))*path=target+path->substr(editing_group_.size());};remap(group_);if(home_)remap(home_->group);
            std::set<std::string> changed;for(const auto& path:folded_)changed.insert(under(path,editing_group_)?target+path.substr(editing_group_.size()):path);folded_=std::move(changed);
        }
        screen_=Screen::Groups;reload();
    },true,28);
}
void App::opml_screen(){
    const int w=c_.width;button({32,8,220,88},"订阅",[this]{back();},false,26,Icon::Back,true,Align::Left);c_.rule(32,104,w-64,30);c_.text(32,174,"OPML",48);
    button({32,270,w-64,112},"导入 OPML",[this]{confirmation_="import";},false,32,Icon::Download,true,Align::Left);
    c_.text(32,428,"从本机 feeds.opml 合并订阅及分组",24,85);
    button({32,510,w-64,112},"导出 OPML",[this]{request_="export";},false,32,Icon::Upload,true,Align::Left);
    c_.text(32,668,"写入 subscriptions.opml，保留空分组",24,85);
    if(!status_.empty())c_.text(32,820,c_.ellipsize(status_,24,w-64),24,75);
}
void App::sync_started(){status_.clear();sync_page_=0;sync_state_=SyncState::Running;sync_completed_=sync_failed_=0;sync_total_=static_cast<int>(feeds_.size());sync_message_="正在连接订阅源";if(screen_==Screen::Sync)dirty=true;}
void App::sync_progress(int completed,int total,int failed,const std::string& message){sync_completed_=completed;sync_total_=total;sync_failed_=failed;sync_message_=message;if(screen_==Screen::Sync)dirty=true;}
void App::sync_finished(bool cancelled,const std::string& error){
    feeds_=store_.feeds();group_names_=store_.groups();sync_failed_=static_cast<int>(std::count_if(feeds_.begin(),feeds_.end(),[](const Feed& f){return !f.error.empty();}));sync_state_=cancelled?SyncState::Cancelled:(!error.empty()||sync_failed_)?SyncState::Failed:SyncState::Complete;
    if(!error.empty())sync_message_=error;
    if(screen_==Screen::Sync)dirty=true;else notify(cancelled?"同步已取消，点击更新列表":sync_state_==SyncState::Failed?"同步有未完成项目，点击更新列表":"同步已完成，点击更新列表");
}
void App::sync_screen(){
    const int w=c_.width,h=c_.height;button({32,8,240,88},"返回列表",[this]{back();},false,26,Icon::Back,true,Align::Left);c_.text(w-120,76,"同步",28,90);c_.rule(32,104,w-64,30);
    const auto heading=sync_state_==SyncState::Running?"正在同步订阅":sync_state_==SyncState::Failed?"部分订阅未能同步":sync_state_==SyncState::Cancelled?"同步已取消":sync_state_==SyncState::Complete?"同步已完成":"同步订阅";
    c_.text(32,174,heading,48);c_.text(32,224,"已缓存文章、收藏与阅读位置会保留。",24,85);
    int y=300;
    if(sync_state_==SyncState::Failed){
        std::vector<Feed> failed;for(const auto& f:feeds_)if(!f.error.empty())failed.push_back(f);
        const std::size_t pages=std::max<std::size_t>(1,(failed.size()+3)/4);sync_page_=std::min(sync_page_,pages-1);nav_page_count_=pages;
        for(std::size_t i=sync_page_*4;i<std::min(failed.size(),(sync_page_+1)*4);++i){const auto f=failed[i];
            button({32,y,w-64,96},f.title.empty()?f.url:f.title,[this,f]{edit_feed(f);},false,30,Icon::Rss,true,Align::Left);
            c_.text(32,y+132,c_.ellipsize(f.error,24,w-64),24,90);y+=180;
        }
        if(failed.empty()&&!sync_message_.empty())for(const auto& line:c_.wrap(sync_message_,26,w-64)){if(y>h-300)break;c_.text(32,y,line.text,26,85);y+=42;}
        if(pages>1){
            button({32,h-204,220,88},"上一页",[this]{--sync_page_;},false,26,Icon::Back,sync_page_>0,Align::Left);
            const auto label=std::to_string(sync_page_+1)+" / "+std::to_string(pages);c_.text(w/2-c_.measure(label,24)/2,h-148,label,24,90);
            button({w-252,h-204,220,88},"下一页",[this]{++sync_page_;},false,26,Icon::Next,sync_page_+1<pages,Align::Right);
        }
    }else{
        frame(c_,{32,y,w-64,22},2);if(sync_total_>0)c_.fill({34,y+2,static_cast<int>(static_cast<long long>(w-68)*sync_completed_/sync_total_),18},30);y+=90;
        if(!sync_message_.empty()){for(const auto& line:c_.wrap(sync_message_,26,w-64)){if(y>h-300)break;c_.text(32,y,line.text,26,85);y+=42;}}
    }
    if(!status_.empty())c_.text(32,h-(sync_state_==SyncState::Failed&&sync_failed_>4?260:152),c_.ellipsize(status_,24,w-64),24,80);
    const auto running=sync_state_==SyncState::Running;
    button({32,h-100,400,88},running?"取消同步":sync_state_==SyncState::Failed?"重试失败订阅":"返回列表",[this,running]{if(running)request_="cancel-sync";else if(sync_state_==SyncState::Failed){request_="retry-sync";sync_started();}else{screen_=Screen::List;status_.clear();reload();}},false,28,running?Icon::Close:sync_state_==SyncState::Failed?Icon::Sync:Icon::Back,true,Align::Left);
    button({w-312,h-100,280,88},"缓存管理",[this]{screen_=Screen::Cache;},false,28,Icon::Settings,true,Align::Right);
}
void App::input_screen(){
    const int w=c_.width,h=c_.height;
    const auto title=input_mode_=="url"?"订阅地址":input_mode_=="title"?"订阅名称":input_mode_=="folder"?"分组名称":input_mode_=="group-find"?"查找分组":"搜索文章";
    std::string hint=input_mode_=="url"?"输入 HTTP(S) RSS / Atom 地址":input_mode_=="title"?"可留空，同步时自动读取博客名称":input_mode_=="folder"?"请输入不含 / 的分组名称":input_mode_=="group-find"?"按分组名称或路径查找":"搜索本地文章标题";
    if(input_mode_=="search")hint+=" · 范围："+(feed_?source(feed_):group_?(group_->empty()?"未分组":*group_):"全部订阅");
    c_.text(32,88,title,44);button({w-212,8,180,88},"取消",[this]{back();},false,26,Icon::Close,true,Align::Right);
    c_.text(32,150,c_.ellipsize(hint,24,w-64),24,75);
    if(input_mode_=="search")hits_.push_back({{32,104,w-64,88},[this]{open_group_picker(PickerPurpose::Search);}});
    c_.rule(32,180,w-64,180);
    const auto frame=[this](Rect r){c_.fill(r,170);c_.fill({r.x+1,r.y+1,r.w-2,r.h-2},255);};
    frame({32,204,w-64,200});
    auto lines=c_.wrap(input_,28,w-112);if(lines.empty())lines.push_back(Line{});
    const auto first=lines.size()>4?lines.size()-4:0;
    for(std::size_t i=first;i<lines.size();++i)c_.text(56,246+static_cast<int>(i-first)*44,lines[i].text,28);
    const int cursor_x=56+c_.measure(lines.back().text,28);
    c_.fill({cursor_x,223+static_cast<int>(lines.size()-first-1)*44,2,28},30);
    c_.text(32,456,input_mode_=="url"?"支持 RSS / Atom，也可通过 OPML 批量导入。":"点击确定应用输入；取消保留原内容。",24,75);
    c_.rule(32,484,w-64,190);
    c_.text(32,528,chinese_?(composing_.empty()?"中文拼音 · ü 用 v":"中文拼音 · "+composing_):symbols_?"英文 · 符号":"英文 · 字母",26,30);
    const auto candidates=chinese_?pinyin_candidates(composing_):std::vector<std::string>{};
    const auto pages=(candidates.size()+5)/6;
    if(candidate_page_>=std::max<std::size_t>(1,pages))candidate_page_=0;
    for(std::size_t i=candidate_page_*6;i<std::min(candidates.size(),(candidate_page_+1)*6);++i){
        const auto slot=i%6;const auto word=candidates[i];const int candidate_width=(w-88)/3;
        const Rect candidate{32+static_cast<int>(slot%3)*(candidate_width+12),548+static_cast<int>(slot/3)*100,candidate_width,88};
        frame(candidate);button(candidate,word,[this,word]{text_input(word);},false,30);
    }
    if(chinese_&&candidates.empty())c_.text(32,610,composing_.empty()?"输入拼音后，点击候选词上屏。":"暂无候选，请修改拼音。",24,100);
    if(pages>1){
        button({32,756,220,88},"上一组",[this]{--candidate_page_;},false,24,Icon::Back,candidate_page_>0,Align::Left);
        const auto page=std::to_string(candidate_page_+1)+" / "+std::to_string(pages);c_.text((w-c_.measure(page,24))/2,810,page,24,100);
        button({w-252,756,220,88},"下一组",[this]{++candidate_page_;},false,24,Icon::Next,candidate_page_+1<pages,Align::Right);
    }
    const std::vector<std::string> keys=symbols_?std::vector<std::string>{"1234567890","@#$%&-+/=",":;!?._~"}:
        std::vector<std::string>{"qwertyuiop","asdfghjkl","zxcvbnm"};
    const int key_width=(w-64-9*6)/10,key_height=104,key_gap=12,top=h-32-4*key_height-3*key_gap;
    int y=top;
    for(const auto& row:keys){
        const int total=static_cast<int>(row.size())*key_width+static_cast<int>(row.size()-1)*6;
        const int start=(w-total)/2;
        for(std::size_t i=0;i<row.size();++i){
            const std::string ch(1,shift_&&!symbols_?static_cast<char>(std::toupper(static_cast<unsigned char>(row[i]))):row[i]);
            const Rect key{start+static_cast<int>(i)*(key_width+6),y,key_width,key_height};
            frame(key);button(key,ch,[this,ch]{keyboard_key(ch);},false,30);
        }
        y+=key_height+key_gap;
    }
    const Rect shift{32,top+2*(key_height+key_gap),144,key_height},erase{w-176,shift.y,144,key_height};
    frame(shift);button(shift,chinese_?"清拼音":shift_?"小写":"大写",[this]{if(chinese_){composing_.clear();candidate_page_=0;}else shift_=!shift_;},false,24,Icon::None,chinese_?!composing_.empty():!symbols_);
    frame(erase);button(erase,"删除",[this]{backspace();},false,26);
    const Rect symbols{32,y,144,key_height},space{192,y,420,key_height},language{628,y,168,key_height},confirm{812,y,w-844,key_height};
    frame(symbols);button(symbols,symbols_?"字母":"符号",[this]{symbols_=!symbols_;},false,26);
    frame(space);button(space,composing_.empty()?"空格":"选首词",[this]{input_space();},false,28);
    frame(language);button(language,input_mode_=="url"?"英文":chinese_?"切换英文":"切换中文",[this]{
        if(!composing_.empty()){const auto raw=composing_;text_input(raw);if(!composing_.empty())return;}
        chinese_=!chinese_;shift_=false;
    },false,24,Icon::None,input_mode_!="url");
    button(confirm,"确定",[this]{submit();},true,28,Icon::Check);
}
bool App::draw_image(const ArticleImage& image,Rect area,bool enlarge,int clip_bottom){
    if(area.w<=0||area.h<=0||image.width<=0||image.height<=0||image.width>1072||image.height>1448||
       image.pixels.size()!=static_cast<std::size_t>(image.width)*image.height)return false;
    const double fitted=std::min(double(area.w)/image.width,double(area.h)/image.height);
    const double scale=enlarge?fitted:std::min(1.0,fitted);
    const int width=std::max(1,static_cast<int>(image.width*scale)),height=std::max(1,static_cast<int>(image.height*scale));
    const int left=area.x+(area.w-width)/2,top=area.y+(area.h-height)/2;
    const int bottom=clip_bottom<0?c_.height:std::min(c_.height,clip_bottom);
    for(int y=std::max(0,-top);y<std::min(height,bottom-top);++y)for(int x=std::max(0,-left);x<std::min(width,c_.width-left);++x)
        c_.pixels[(top+y)*c_.width+left+x]=image.pixels[(y*image.height/height)*image.width+x*image.width/width];
    return true;
}
void App::edit_feed(const Feed& feed){
    status_.clear();
    if(screen_==Screen::List){home_=ListContext{view_,feed_,group_,search_,list_page_};source_list_=false;}
    edit_origin_=screen_==Screen::Sync?Screen::Sync:Screen::Subscriptions;screen_=edit_origin_;managing_=feed;
    if(managing_->url.empty())managing_->url="https://";
}
void App::save_feed(){
    if(!managing_)return;auto draft=*managing_;draft.title=trim(draft.title);draft.url=trim(draft.url);
    if(draft.title.size()>512||std::any_of(draft.title.begin(),draft.title.end(),[](unsigned char c){return c<32||c==127;}))throw std::runtime_error("订阅名称须为不超过 512 字节的单行文字，可留空");
    if(draft.id){
        auto original=std::find_if(feeds_.begin(),feeds_.end(),[&](const Feed& f){return f.id==draft.id;});
        if(original!=feeds_.end()&&sync_state_==SyncState::Running&&(draft.url!=original->url||(draft.title.empty()&&!original->title.empty())))throw std::runtime_error("请先取消同步，再修改订阅地址或清空名称");
        store_.update_feed(draft);
    }else{
        if(std::any_of(feeds_.begin(),feeds_.end(),[&](const Feed& f){return f.url==draft.url;}))throw std::runtime_error("此订阅地址已存在");
        store_.add_feed(draft);
    }
    managing_.reset();screen_=edit_origin_;reload();notify("订阅更改已保存");
}
void App::management_screen(){
    const int w=c_.width,h=c_.height;const bool storage=screen_==Screen::Cache&&!managing_;
    button({32,8,240,88},storage?"同步":edit_origin_==Screen::Sync?"同步":"订阅",[this]{back();},false,26,Icon::Back,true,Align::Left);c_.rule(32,104,w-64,30);
    c_.text(32,174,storage?"缓存管理":managing_->id?"编辑订阅":"添加订阅",48);
    if(storage){
        const auto entries=store_.entries(View::All);const auto cutoff=static_cast<std::int64_t>(std::time(nullptr))-30LL*86400;
        const auto starred=std::count_if(entries.begin(),entries.end(),[](const Entry& e){return e.starred;});
        const auto eligible=std::count_if(entries.begin(),entries.end(),[&](const Entry& e){return e.read&&!e.starred&&!e.in_progress&&e.published<cutoff;});
        c_.text(32,300,"本地文章  "+std::to_string(entries.size())+" 篇",32);
        c_.text(32,380,"已收藏    "+std::to_string(starred)+" 篇",32);
        c_.text(32,460,"可清理    "+std::to_string(eligible)+" 篇",32);
        c_.rule(32,520,w-64,180);c_.text(32,592,"清理 30 天前的已读文章",32);
        int y=680;for(const auto& line:c_.wrap("收藏、未读与正在阅读的文章会保留。\n\n清理会删除这些文章的本地内容和状态；订阅源仍提供的文章可能在同步后重新出现。",28,w-64)){c_.text(32,y,line.text,28,85);y+=44;}
        button({32,h-100,440,88},"清理旧已读缓存",[this]{confirmation_="clean";},false,28,Icon::Download,true,Align::Left);return;
    }
    c_.text(32,254,"名称（选填）",24,85);frame(c_,{32,280,w-64,112});
    button({56,288,w-112,96},managing_->title.empty()?"留空，同步时自动读取博客名称":managing_->title,[this]{start_input("title",managing_->title);},false,30,Icon::None,true,Align::Left);
    c_.text(32,442,"RSS 地址",24,85);frame(c_,{32,466,w-64,160});auto url=c_.wrap(managing_->url,26,w-112);
    for(std::size_t i=0;i<std::min<std::size_t>(3,url.size());++i)c_.text(56,508+static_cast<int>(i)*38,c_.ellipsize(url[i].text+(i==2&&url.size()>3?"…":""),26,w-112),26);
    hits_.push_back({{32,466,w-64,160},[this]{start_input("url",managing_->url);}});
    c_.text(32,700,"所属分组",24,85);frame(c_,{32,724,w-64,96});
    button({56,728,w-112,88},group_label(managing_->group),[this]{open_group_picker(PickerPurpose::Move);},false,30,Icon::Folder,true,Align::Left);
    if(managing_->id){
        button({32,900,320,88},"取消订阅",[this]{confirmation_="remove";},false,28,Icon::Close,true,Align::Left);
        button({w-452,900,420,88},"本订阅标为已读",[this]{confirmation_="read";},false,26,Icon::Check,true,Align::Right);
        c_.text(32,1040,"取消订阅仅保留收藏文章；两项操作均需确认。",24,85);
    }
    if(!status_.empty())c_.text(32,h-152,c_.ellipsize(status_,24,w-64),24,80);
    button({32,h-100,320,88},"放弃更改",[this]{managing_.reset();screen_=edit_origin_;},false,28,Icon::None,true,Align::Left);
    button({w-312,h-100,280,88},"保存更改",[this]{save_feed();},true,28);
}
void App::confirmation_screen(){
    const int w=c_.width,h=c_.height;
    if(confirmation_=="exit"){
        list_screen();hits_.clear();hits_.push_back({{0,0,w,h},[this]{confirmation_.clear();}});
        const Rect dialog{32,240,w-64,320};frame(c_,dialog,2);c_.text(dialog.x+24,dialog.y+78,"退出 RSS Reader？",40);
        c_.text(dialog.x+24,dialog.y+148,"订阅、收藏与阅读位置都会保留。",28);
        button({dialog.x+24,dialog.y+200,320,88},"取消",[this]{confirmation_.clear();},false,28);
        button({dialog.x+dialog.w-344,dialog.y+200,320,88},"退出应用",[this]{confirmation_.clear();quit=true;},true,28);return;
    }
    button({32,8,220,88},"返回",[this]{confirmation_.clear();},false,26,Icon::Back,true,Align::Left);c_.rule(32,104,w-64,30);
    const auto message=confirmation_=="remove"?"取消这个订阅？\n\n收藏文章会保留。其余文章及阅读进度将从本机删除。":
        confirmation_=="read"?"将这个订阅的全部文章标为已读？\n\n收藏和阅读位置会保留。":
        confirmation_=="delete-group"?"删除这个分组？\n\n此分组及子分组会移除，组内订阅移到未分组，所有文章和状态保留。":
        confirmation_=="import"?"导入本地 OPML？\n\n从 feeds.opml 合并订阅与分组；重复地址不会再次添加。":
        "清理 30 天前的已读文章？\n\n收藏、未读和正在阅读的文章会保留。此操作无法撤销。";
    int y=250;for(const auto& line:c_.wrap(message,32,w-64)){c_.text(32,y,line.text,32);y+=50;}
    button({32,h-100,280,88},"取消",[this]{confirmation_.clear();},false,28,Icon::None,true,Align::Left);
    button({w-312,h-100,280,88},"确认",[this]{
        std::string result;const auto kind=confirmation_;
        if(kind=="remove"){
            const auto id=managing_->id;store_.remove_feed(id);if(feed_==id){feed_=0;group_.reset();source_list_=false;view_=View::Unread;search_.clear();list_page_=0;}
            if(home_&&home_->feed==id){home_->feed=0;home_->group.reset();home_->search.clear();home_->page=0;}
            managing_.reset();screen_=Screen::Subscriptions;result="已取消订阅，收藏文章已保留";
        }else if(kind=="read"){
            store_.mark_feed_read(managing_->id);result="本订阅已全部标为已读";
        }else if(kind=="delete-group"){
            store_.remove_group(editing_group_);if(group_&&under(*group_,editing_group_))group_=std::string{};
            if(home_&&home_->group&&under(*home_->group,editing_group_))home_->group=std::string{};
            for(auto it=folded_.begin();it!=folded_.end();)if(under(*it,editing_group_))it=folded_.erase(it);else ++it;
            screen_=Screen::Groups;result="分组已删除，订阅已移到未分组";
        }else if(kind=="import"){request_="import";screen_=Screen::Opml;}
        else{const auto count=store_.clean_read(static_cast<std::int64_t>(std::time(nullptr))-30LL*86400);result="已清理 "+std::to_string(count)+" 篇旧文章";}
        confirmation_.clear();reload();
        if(kind=="remove"&&sync_state_==SyncState::Failed&&std::none_of(feeds_.begin(),feeds_.end(),[](const Feed& f){return !f.error.empty();})){sync_state_=SyncState::Complete;sync_message_="失败订阅已移除";}
        if(!result.empty())notify(result);
    },true,28);
}
void App::start_input(const std::string& mode,const std::string& value){
    input_mode_=mode;input_=value;composing_.clear();candidate_page_=0;chinese_=mode!="url";shift_=false;symbols_=false;
}
void App::text_input(const std::string& value){
    if(!error_.empty()||input_mode_.empty())return;
    if(value.size()>2048||input_.size()>2048-value.size()){show_error("输入内容过长，请删除部分文字后重试。");return;}
    input_+=value;composing_.clear();candidate_page_=0;dirty=true;
}
void App::keyboard_key(const std::string& value){
    if(chinese_&&value.size()==1&&value[0]>='a'&&value[0]<='z'){
        if(composing_.size()<32){composing_+=value;candidate_page_=0;}
    }else if(composing_.empty())text_input(value);
    else show_error("请先选择候选词，或清空拼音。");
}
void App::input_space(){
    if(composing_.empty()){text_input(" ");return;}
    const auto candidates=pinyin_candidates(composing_);
    if(!candidates.empty())text_input(candidates.front());
    else show_error("暂无候选，请修改拼音。");
}
void App::backspace(){
    if(!error_.empty())return;
    if(!composing_.empty()){composing_.pop_back();candidate_page_=0;dirty=true;return;}
    if(input_.empty())return;auto i=input_.size()-1;
    while(i>0&&(static_cast<unsigned char>(input_[i])&0xc0)==0x80)--i;
    input_.erase(i);dirty=true;
}
void App::clear_search(){search_.clear();input_mode_.clear();input_.clear();composing_.clear();candidate_page_=0;list_page_=0;screen_=Screen::List;reload();}
void App::submit(){
    if(!error_.empty())return;if(!composing_.empty()){show_error("请先选择候选词，再点击确定。");return;}
    const auto value=trim(input_);
    if(input_mode_=="title"||input_mode_=="url"){
        if(input_mode_=="url"&&value.empty())throw std::runtime_error("订阅地址不能为空");
        if(input_mode_=="title")managing_->title=value;else managing_->url=value;input_mode_.clear();
    }else if(input_mode_=="folder"){group_name_draft_=value;input_mode_.clear();}
    else if(input_mode_=="group-find"){picker_query_=value;picker_page_=0;input_mode_.clear();screen_=Screen::GroupPicker;}
    else if(input_mode_=="search"){
        if(value.empty())clear_search();else{search_=value;view_=View::All;input_mode_.clear();list_page_=0;screen_=Screen::List;reload();}
    }
    dirty=true;
}
void App::tap(int x,int y){
    for(auto it=hits_.rbegin();it!=hits_.rend();++it)if(it->rect.contains(x,y)){
        last_tap_x_=x;auto action=it->action;action();dirty=true;return;
    }
}
void App::next(){
    if(!input_mode_.empty()||!confirmation_.empty()||(managing_&&screen_!=Screen::GroupPicker)||brand_menu_)return;
    if(current_&&reader_panel_==ReaderPanel::Images){if(image_page_+1<current_->images.size())++image_page_;}
    else if(current_&&!reader_tools_){if(page_+1<pages_.size()){++page_;save_position();}}
    else if(!current_){auto* nav=screen_==Screen::Subscriptions?&nav_page_:screen_==Screen::Groups?&groups_page_:screen_==Screen::GroupPicker?&picker_page_:screen_==Screen::Sync&&sync_state_==SyncState::Failed?&sync_page_:nullptr;
        if(nav){if(*nav+1<nav_page_count_)++*nav;}else if(screen_==Screen::List){ensure_list_page(list_page_+1);if(list_page_+1<list_starts_.size())++list_page_;}}
    dirty=true;
}
void App::previous(){
    if(!input_mode_.empty()||!confirmation_.empty()||(managing_&&screen_!=Screen::GroupPicker)||brand_menu_)return;
    if(current_&&reader_panel_==ReaderPanel::Images){if(image_page_)--image_page_;}
    else if(current_&&!reader_tools_){if(page_){--page_;save_position();}}
    else if(!current_){auto* nav=screen_==Screen::Subscriptions?&nav_page_:screen_==Screen::Groups?&groups_page_:screen_==Screen::GroupPicker?&picker_page_:screen_==Screen::Sync&&sync_state_==SyncState::Failed?&sync_page_:nullptr;
        if(nav){if(*nav)--*nav;}else if(screen_==Screen::List&&list_page_)--list_page_;}
    dirty=true;
}
void App::back(){
    if(!error_.empty())error_.clear();
    else if(!confirmation_.empty())confirmation_.clear();
    else if(brand_menu_)brand_menu_=false;
    else if(!input_mode_.empty()){input_mode_.clear();input_.clear();composing_.clear();}
    else if(screen_==Screen::GroupPicker)restore_input();
    else if(managing_){managing_.reset();screen_=edit_origin_;}
    else if(screen_==Screen::GroupEdit)screen_=Screen::Groups;
    else if(current_&&reader_panel_!=ReaderPanel::None){reader_panel_=ReaderPanel::None;image_zoom_=false;typography_anchor_.reset();}
    else if(current_&&reader_tools_)close_reader_tools();
    else if(current_){save_position();current_.reset();pages_.clear();}
    else if(screen_==Screen::Subscriptions)restore_home();
    else if(screen_==Screen::Groups||screen_==Screen::Opml)screen_=Screen::Subscriptions;
    else if(screen_==Screen::Cache)screen_=Screen::Sync;
    else if(screen_==Screen::Sync){screen_=Screen::List;if(sync_state_!=SyncState::Running){status_.clear();reload();}}
    else if(source_list_)screen_=Screen::Subscriptions;
    dirty=true;
}
void App::notify(const std::string& text){status_=text;list_starts_={0};list_complete_=false;ensure_list_page(list_page_);list_page_=std::min(list_page_,list_starts_.size()-1);dirty=true;}
void App::show_error(const std::string& text){error_=text;dirty=true;}
std::string App::take_request(){auto request=request_;request_.clear();return request;}
}
