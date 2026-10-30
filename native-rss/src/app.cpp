#include "app.hpp"
#include "pinyin.hpp"
#include "feed.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <set>
#include <tuple>

namespace rss {
namespace {
constexpr int reader_bar_height=96;
constexpr int reader_content_top=96,reader_footer_space=72;
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
void frame(Canvas& c,Rect r,int weight=1){c.fill(r,weight>1?30:150);c.fill({r.x+weight,r.y+weight,r.w-2*weight,r.h-2*weight},255);}
void modal_backdrop(Canvas& c){
    for(std::size_t i=static_cast<std::size_t>(76)*c.width;i<c.pixels.size();++i)c.pixels[i]=192+c.pixels[i]/4;
}
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
App::App(Store& store,Canvas& canvas,std::int64_t initial_article,const std::string& status_font):store_(store),c_(canvas){
    if(!status_font.empty())try{status_canvas_=std::make_unique<Canvas>(c_.width,100,status_font);}
    catch(const std::exception&){std::fprintf(stderr,"RSS Reader: status font unavailable; using default\n");}
    font_size_=read_int(store_,"font_size",34,24,48);
    line_gap_=read_int(store_,"line_gap",24,10,42);
    margin_=read_int(store_,"margin",32,32,100);
    cre_requested_=store_.setting("reader_engine")!="classic"&&CreReader::available();
    reload();
    if(initial_article){try{open(initial_article);}catch(const std::exception& e){show_error(e.what());}}
}
int App::row_height(const Entry& e){
    const int width=c_.width-108;const auto metadata=article_metadata(e);
    auto found=row_metrics_.find(e.id);
    if(found==row_metrics_.end()||found->second.width!=width||found->second.title!=e.title||found->second.metadata!=metadata){
        // shortcut: over 10,000 unique rows can recompute metrics; use LRU if large libraries become common.
        if(found==row_metrics_.end()&&row_metrics_.size()>=10000)row_metrics_.clear();
        found=row_metrics_.insert_or_assign(e.id,RowMetrics{e.title,metadata,width,
            static_cast<int>(c_.wrap(e.title,36,width).size()),static_cast<int>(std::min<std::size_t>(2,c_.wrap(metadata,24,width).size()))}).first;
    }
    const auto& metrics=found->second;
    const int cap=std::max(1,(list_bottom()-list_top()-metrics.meta_lines*32-48)/46);
    return std::max(160,std::min(cap,metrics.title_lines)*46+metrics.meta_lines*32+48);
}
void App::ensure_list_page(std::size_t page){
    const int top=list_top(),bottom=list_bottom();
    if(list_layout_width_!=c_.width||list_layout_top_!=top||list_layout_bottom_!=bottom){
        list_starts_={0};list_complete_=false;
        list_layout_width_=c_.width;list_layout_top_=top;list_layout_bottom_=bottom;
    }
    while(!list_complete_&&list_starts_.size()<=page){
        const auto start=list_starts_.back();int y=top;std::size_t i=start;
        for(;i<list_.size();++i){const int height=row_height(list_[i]);if(y+height>bottom&&i>start)break;y+=height;}
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
std::string App::scope_name()const{
    if(feed_)return source(feed_);
    if(group_)return group_label(*group_);
    return "全部订阅";
}

void App::button(Rect r,const std::string& label,std::function<void()> action,bool filled,int size,Icon icon,bool enabled,Align align){
    if(filled)c_.fill(r,30);
    const int symbol=icon==Icon::None?0:28,gap=symbol?12:0;
    const auto text=c_.ellipsize(label,size,r.w-20-symbol-gap);
    const int content=c_.measure(text,size)+symbol+gap;
    const int left=align==Align::Left?r.x:align==Align::Right?r.x+r.w-content:r.x+(r.w-content)/2;
    const auto color=filled?255:enabled?30:130;
    const bool trailing=icon==Icon::Next;
    if(symbol)c_.icon(icon,{trailing?left+c_.measure(text,size)+gap:left,r.y+(r.h-symbol)/2,symbol,symbol},color);
    const auto bounds=c_.text_vertical_bounds(text,size);
    c_.text(trailing?left:left+symbol+gap,r.y+(r.h-(bounds.second-bounds.first))/2-bounds.first,text,size,color);
    if(enabled)hits_.push_back({r,std::move(action)});
}
void App::back_button(int top,int height){
    c_.icon(Icon::BackCircle,{42,top+(height-40)/2,40,40},30);
    hits_.push_back({{24,top,96,height},[this]{back();}});
}
void App::page_header(const std::string& title){
    back_button();c_.rule(32,76,c_.width-64,150);c_.text(32,154,title,48);
}
void App::draw(){
    hits_.clear();c_.clear();bool reader_visible=false,list_visible=false;
    if(!error_.empty()){
        page_header("操作未完成");int y=250;
        for(const auto& line:c_.wrap(error_,28,c_.width-64)){if(y>c_.height-220)break;c_.text(32,y,line.text,28);y+=44;}
        c_.rule(32,c_.height-104,c_.width-64,205);
        button({32,c_.height-100,c_.width-64,88},"返回",[this]{back();},true,28);
    }else if(!confirmation_.empty()){confirmation_screen();list_visible=confirmation_=="exit";}
    else if(!input_mode_.empty())input_screen();
    else if(screen_==Screen::GroupPicker){if(picker_purpose_==PickerPurpose::Read){list_screen();list_visible=true;}group_picker_screen();}
    else if(managing_)management_screen();
    else if(screen_==Screen::GroupEdit)group_edit_screen();
    else if(current_){reader_visible=true;reader_screen();if(reader_tools_)reader_tools();if(reader_panel_!=ReaderPanel::None)reader_panel_screen();}
    else if(screen_==Screen::Subscriptions||screen_==Screen::Groups)nav_screen();
    else if(screen_==Screen::Opml)opml_screen();
    else if(screen_==Screen::Sync){list_screen();list_visible=true;sync_screen();}
    else if(screen_==Screen::Cache)management_screen();
    else if(screen_==Screen::Settings)settings_screen();
    else {list_screen();list_visible=true;if(brand_menu_)app_menu();}
    if(!reader_visible)device_status(true,list_visible);
    dirty=false;
}
void App::device_status(bool clock,bool clock_left){
    constexpr int top=17,size=31;
    auto& status=status_canvas_?*status_canvas_:c_;
    if(status_canvas_)status.clear();
    const int w=c_.width,baseline=top+size;
    if(clock){const auto time=reader_status_.clock.empty()?"--:--":reader_status_.clock;status.text(clock_left?32:(w-status.measure(time,size))/2,baseline,time,size,0);}
    const auto battery=reader_status_.battery<0?"--%":std::string(reader_status_.charging?"+":"")+std::to_string(reader_status_.battery)+"%";
    const int text_x=w-34-status.measure(battery,size),battery_x=text_x-49;
    if(reader_status_.wifi)status.icon(Icon::Wifi,{battery_x-47,top-4,40,40},0);
    status.icon(Icon::Battery,{battery_x-3,top-4,48,48},0);
    if(reader_status_.battery>=0)status.fill({battery_x+4,top+13,std::clamp(reader_status_.battery,0,100)*30/100,13},0);
    status.text(text_x,baseline,battery,size,0);
    if(status_canvas_){
        const int rows=std::min(status.height,c_.height);
        for(int y=0;y<rows;++y)for(int x=0;x<w;++x){
            const auto pixel=status.pixels[static_cast<std::size_t>(y)*w+x];
            if(pixel!=255)c_.pixels[static_cast<std::size_t>(y)*w+x]=pixel;
        }
    }
}
void App::list_screen(){
    // Cache all page boundaries once so the first footer includes the exact total.
    ensure_list_page(std::max<std::size_t>(1,list_.size()));list_page_=std::min(list_page_,list_starts_.size()-1);
    const int w=c_.width,h=c_.height,m=32;
    const auto total=view_==View::Unread?std::to_string(store_.entry_count(view_,feed_,search_,midnight(),group_)):std::to_string(list_.size());
    const int filters=w-492,scope_width=filters-m-24;
    const auto scope=c_.ellipsize(scope_name(),40,scope_width-44);
    c_.text(m,124,scope,40);down(c_,m+c_.measure(scope,40)+20,104);
    hits_.push_back({{m,68,scope_width,96},[this]{open_group_picker(PickerPurpose::Read);}});
    const std::vector<std::pair<std::string,View>> choices={{"未读",View::Unread},{"全部",View::All},{"收藏",View::Starred}};
    for(std::size_t i=0;i<choices.size();++i){int x=filters+static_cast<int>(i)*116;bool selected=view_==choices[i].second;
        c_.text(x+(116-c_.measure(choices[i].first,32))/2,124,choices[i].first,32,selected?20:90);
        if(selected)c_.fill({x+14,160,88,4},25);
        hits_.push_back({{x,68,116,96},[this,v=choices[i].second]{view_=v;list_page_=0;reload();}});
    }
    c_.icon(Icon::Search,{w-94,94,36,36});
    hits_.push_back({{w-120,68,88,96},[this]{start_input("search",search_);}});
    c_.rule(m,164,w-64,205);
    if(!search_.empty()){
        c_.text(m,220,c_.ellipsize("搜索："+search_,28,w-276),28,85);
        button({w-212,164,180,88},"退出搜索",[this]{clear_search();},false,28);
        c_.rule(m,list_top(),w-64,150);
    }
    if(list_.empty()){
        const bool none=feeds_.empty()&&search_.empty(),empty_search=!search_.empty(),empty_star=view_==View::Starred;
        const bool done=view_==View::Unread&&!store_.entries(View::All,feed_,"",midnight(),group_).empty();
        const auto title=none?"从第一份订阅开始":empty_search?"没有找到相关标题":empty_star?"还没有收藏":done?"未读已全部读完":"这里还没有文章";
        const std::string copy=none?"添加 RSS 地址，或导入 OPML 文件。内容缓存后，即可离线阅读。":
            empty_search?"试试更短的关键词，或切换搜索范围。":
            empty_star?"点正文中央展开工具，再点星标收藏文章。":done?"慢慢读，也可以就此休息。已读文章仍可在「全部」中找到。":"同步后，已缓存的文章会显示在这里。";
        const std::string label=none?"添加订阅":empty_search?"修改搜索":done?"查看全部文章":"订阅";
        auto lines=c_.wrap(copy,28,w-64);
        const int fixed_height=104+56+32+88;
        const int available=list_bottom()-list_top(),max_lines=std::max(1,(available-64-fixed_height)/44);
        if(lines.size()>static_cast<std::size_t>(max_lines)){lines.resize(max_lines);lines.back().text=c_.ellipsize(lines.back().text+"…",28,w-64);}
        const int content_height=fixed_height+static_cast<int>(lines.size())*44;
        int y=list_top()+std::max(32,(available-content_height)/2);
        c_.icon(none?Icon::Rss:empty_search?Icon::Search:empty_star?Icon::Star:done?Icon::Check:Icon::Folder,{32,y,72,72},30);
        y+=104;c_.text(32,y+36,title,36);y+=56;
        for(const auto& line:lines){c_.text(32,y+28,line.text,28,85);y+=44;}
        y+=32;const Rect action{32,y,std::max(160,c_.measure(label,28)+56),88};
        button(action,label,[this,none,empty_search,done]{
            if(none)edit_feed(Feed{});
            else if(empty_search)start_input("search",search_);
            else if(done){view_=View::All;list_page_=0;reload();}
            else open_subscriptions();
        },true,28);
    }
    int y=list_top();const auto end=list_page_+1<list_starts_.size()?list_starts_[list_page_+1]:list_.size();
    for(auto i=list_starts_[list_page_];i<end;++i){auto& e=list_[i];const int height=row_height(e);
        if(auto fresh=store_.entry(e.id,false)){e.read=fresh->read;e.starred=fresh->starred;}
        auto meta=c_.wrap(article_metadata(e),24,w-108);const int meta_count=static_cast<int>(std::min<std::size_t>(2,meta.size()));
        auto title=c_.wrap(e.title,36,w-108);const int count=std::min<int>(title.size(),std::max(1,(height-meta_count*32-48)/46));
        int ink_top=0,ink_bottom=0;bool has_ink=false;
        const auto include=[&](const std::string& text,int size,int baseline){
            const auto bounds=c_.text_vertical_bounds(text,size);if(bounds.first==bounds.second)return;
            ink_top=has_ink?std::min(ink_top,baseline+bounds.first):baseline+bounds.first;
            ink_bottom=has_ink?std::max(ink_bottom,baseline+bounds.second):baseline+bounds.second;has_ink=true;
        };
        for(int n=0;n<count;++n){if(n==count-1&&count<static_cast<int>(title.size()))title[n].text=c_.ellipsize(title[n].text+"…",36,w-108);include(title[n].text,36,n*46);}
        const int meta_start=(count-1)*46+42;
        for(int n=0;n<meta_count;++n){if(n==meta_count-1&&meta.size()>2)meta[n].text=c_.ellipsize(meta[n].text+"…",24,w-108);include(meta[n].text,24,meta_start+n*32);}
        const int baseline=y+(height-(ink_bottom-ink_top))/2-ink_top;
        if(e.starred)c_.icon(Icon::StarFilled,{m+1,baseline-26,26,26});else if(!e.read)c_.text(m+7,baseline-3,"•",24,20);
        for(int n=0;n<count;++n)c_.text(m+44,baseline+n*46,title[n].text,36,e.read?65:20);
        for(int n=0;n<meta_count;++n)c_.text(m+44,baseline+meta_start+n*32,meta[n].text,24,85);
        c_.rule(m,y+height-1,w-64,205);hits_.push_back({{m,y,w-64,height},[this,id=e.id]{open(id);}});y+=height;
    }
    if(!status_.empty())button({m,h-204,w-64,88},status_,[this]{status_.clear();reload();},false,24);
    c_.rule(32,h-104,w-64,205);
    if(brand_menu_)c_.fill({32,h-100,176,88},30);
    const auto menu_color=brand_menu_?255:30;
    for(int row=0;row<3;++row)c_.fill({42,h-68+row*10,28,2},menu_color);
    const auto menu_bounds=c_.text_vertical_bounds("菜单",28);
    c_.text(88,h-56-(menu_bounds.first+menu_bounds.second)/2,"菜单",28,menu_color);
    hits_.push_back({{32,h-100,176,88},[this]{brand_menu_=true;}});
    button({w-344,h-100,148,88},"上页",[this]{previous();},false,28,Icon::Back,list_page_>0);
    const auto page="第 "+std::to_string(list_page_+1)+" / "+std::to_string(list_starts_.size())+" 页";
    const auto count="共 "+total+" 篇",footer=page+" · "+count;
    if(c_.measure(footer,24)<=w-736)c_.text(w/2-c_.measure(footer,24)/2,h-42,footer,24,90);
    else{
        c_.text(w/2-c_.measure(page,24)/2,h-58,page,24,90);
        c_.text(w/2-c_.measure(count,24)/2,h-26,count,24,90);
    }
    button({w-172,h-100,140,88},"下页",[this]{next();},false,28,Icon::Next,list_page_+1<list_starts_.size());
}
int App::article_heading_height(){
    const int size=std::min(48,font_size_+6);
    return (size+10)*static_cast<int>(std::min<std::size_t>(3,c_.wrap(current_->title,size,c_.width-2*margin_).size()))+64;
}
std::string App::reader_key(const std::string& prefix)const{
    return prefix+(current_->web_content?"web:":"")+std::to_string(current_->id);
}
void App::paginate(){
    if(!current_)return;
    if(cre_){
        try{
            cre_->layout(c_.width-2*margin_,c_.height-reader_content_top-reader_footer_space,font_size_,line_gap_);
            const auto count=cre_->page_count();
            if(!count)throw std::runtime_error("新版排版没有可显示的页面");
            pages_.assign(count,Page{});
            page_=std::min(cre_->restore(store_.setting(reader_key("reader_cre:"))),pages_.size()-1);
            return;
        }catch(const std::exception& e){cre_.reset();show_error(std::string("新版排版无法重排，已回到原排版：")+e.what());}
    }
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
    pages_=c_.paginate_article(current_->text,inline_images,font_size_,line_height(),c_.width-2*margin_,c_.height-reader_content_top-reader_footer_space,
                               c_.height-reader_content_top-reader_footer_space-article_heading_height());
    // Existing reading positions count the title prefix; images consume no text bytes.
    const auto prefix=current_->title.size()+2;
    for(auto& page:pages_){page.start+=prefix;page.end+=prefix;}
    pages_.front().start=0;
    const auto position=static_cast<std::size_t>(std::max<std::int64_t>(0,current_->position));
    page_=page_at(pages_,position);
    const auto anchor=store_.setting(reader_key("reader_image:"));
    for(std::size_t i=0;i<pages_.size();++i)
        if(pages_[i].image_anchor>=0&&anchor==std::to_string(position)+":"+std::to_string(pages_[i].image_anchor)){page_=i;break;}

}
void App::open(std::int64_t id,bool track){
    auto entry=store_.entry(id);if(!entry)throw std::runtime_error("文章已不在缓存中");
    const auto selected=store_.setting("reader_content:"+std::to_string(id));
    if(selected=="web"||(selected.empty()&&!entry->read&&!entry->in_progress)){
        if(auto web=store_.fulltext_entry(id))entry=std::move(web);
    }
    if(entry->text.empty()&&entry->images.empty())throw std::runtime_error("这篇文章尚无可阅读的正文");
    cre_.reset();current_=std::move(entry);
    const bool keep_classic_position=!current_->web_content&&store_.setting("reader_engine").empty()&&current_->position>0&&
        store_.setting("reader_cre:"+std::to_string(id)).empty();
    if(cre_requested_&&!keep_classic_position){
        try{
            std::string base=current_->url;
            if(base.empty())for(const auto& feed:feeds_)if(feed.id==current_->feed_id){base=feed.url;break;}
            cre_=std::make_unique<CreReader>(store_,c_.font_path());
            cre_->open(*current_,base,source(current_->feed_id)+"  ·  "+date(current_->published),
                       c_.width-2*margin_,c_.height-reader_content_top-reader_footer_space,font_size_,line_gap_);
            paginate();
        }catch(const std::exception& e){cre_.reset();paginate();show_error(std::string("新版排版无法打开，已回到原排版：")+e.what());}
    }else paginate();
    if(cre_requested_&&keep_classic_position)notify("已保留原排版阅读位置，可在排版中切换新版");
    if(track){
        try{
            store_.begin_reading(id,current_->position,current_->web_content);
            current_->read=true;
        }catch(...){
            // Dismissing a failed open must return to the list, not a partially opened article.
            cre_.reset();current_.reset();pages_.clear();reader_tools_=false;reader_panel_=ReaderPanel::None;
            throw;
        }
    }
    reader_tools_=false;reader_panel_=ReaderPanel::None;image_zoom_=false;image_page_=0;info_scroll_=0;typography_anchor_.reset();dirty=true;
}
void App::save_position(){
    if(!current_||pages_.empty())return;
    if(cre_){
        store_.set_setting(reader_key("reader_cre:"),cre_->anchor(page_));
        store_.set_position(current_->id,current_->position,current_->web_content);
        return;
    }
    current_->position=static_cast<std::int64_t>(pages_[page_].start);
    store_.set_position(current_->id,current_->position,current_->web_content);
    // Consecutive image-only pages share a text offset; retain their occurrence separately.
    store_.set_setting(reader_key("reader_image:"),
                       std::to_string(current_->position)+":"+std::to_string(pages_[page_].image_anchor));
}
void App::set_reader_content(bool web){
    if(!current_||current_->web_content==web)return;
    const auto id=current_->id;
    const auto next=web?store_.fulltext_entry(id):store_.entry(id);
    if(!next||(next->text.empty()&&next->images.empty())){show_error("所选正文尚无可阅读内容");return;}
    save_position();
    store_.set_setting("reader_content:"+std::to_string(id),web?"web":"rss");
    open(id);
    reader_tools_=true;reader_panel_=ReaderPanel::Content;
}
void App::set_reader_engine(bool use_cre){
    if(!current_||use_cre==static_cast<bool>(cre_))return;
    save_position();
    try{
        if(use_cre){
            std::string base=current_->url;
            if(base.empty())for(const auto& feed:feeds_)if(feed.id==current_->feed_id){base=feed.url;break;}
            auto next=std::make_unique<CreReader>(store_,c_.font_path());
            next->open(*current_,base,source(current_->feed_id)+"  ·  "+date(current_->published),
                       c_.width-2*margin_,c_.height-reader_content_top-reader_footer_space,font_size_,line_gap_);
            const auto restored=next->restore(store_.setting(reader_key("reader_cre:")));
            std::vector<Page> pages(next->page_count());
            if(pages.empty())throw std::runtime_error("新版排版没有可显示的页面");
            store_.set_setting("reader_engine","cre");
            cre_=std::move(next);pages_=std::move(pages);page_=std::min(restored,pages_.size()-1);cre_requested_=true;
        }else{store_.set_setting("reader_engine","classic");cre_.reset();cre_requested_=false;paginate();}
        typography_anchor_.reset();
    }catch(const std::exception& e){show_error(std::string("排版切换失败，已保留当前阅读位置：")+e.what());}
}
void App::reader_screen(){
    const int w=c_.width,h=c_.height;
    back_button(8,88);
    if(reader_tools_){
        button({w-262,4,230,88},current_->starred?"已收藏":"收藏",[this]{
            current_->starred=!current_->starred;store_.set_starred(current_->id,current_->starred);
        },false,28,current_->starred?Icon::StarFilled:Icon::Star,true,Align::Right);
        if(reader_panel_!=ReaderPanel::None)c_.text(144,57,c_.ellipsize(current_->title,24,w-440),24,85);
    }else device_status(false);
    const int heading=cre_?0:page_==0?article_heading_height():0;
    const int top=reader_content_top+heading;
    const int cover_top=reader_panel_==ReaderPanel::None?(reader_tools_?h-reader_bar_height:h-72):h-reader_bar_height-reader_panel_height();
    if(!cre_&&page_==0){
        const int size=std::min(48,font_size_+6);
        const auto title=c_.wrap(current_->title,size,w-2*margin_);
        for(std::size_t i=0;i<std::min<std::size_t>(3,title.size());++i)
            c_.text(margin_,reader_content_top+size+static_cast<int>(i)*(size+10),i==2&&title.size()>3?c_.ellipsize(title[i].text+"…",size,w-2*margin_):title[i].text,size,15);
        const auto meta=source(current_->feed_id)+"  ·  "+date(current_->published);
        c_.text(margin_,reader_content_top+static_cast<int>(std::min<std::size_t>(3,title.size()))*(size+10)+32,c_.ellipsize(meta,24,w-2*margin_),24,75);
    }
    if(cre_)cre_->draw(c_,page_,margin_,reader_content_top,cover_top);
    if(!cre_)for(const auto& item:pages_[page_].elements){
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
        const auto clock=reader_status_.clock.empty()?"--:--":reader_status_.clock;
        c_.text(32,h-20,clock,31,80);
        const auto it=std::find_if(list_.begin(),list_.end(),[this](const Entry& e){return e.id==current_->id;});
        const auto progress=it==list_.end()?source(current_->feed_id):
            "第 "+std::to_string(it-list_.begin()+1)+" / "+std::to_string(list_.size())+" 篇";
        const auto label=c_.ellipsize(progress,28,300);
        const auto footer=std::to_string(page_+1)+" / "+std::to_string(pages_.size());
        c_.text((w-c_.measure(footer,28))/2,h-20,footer,28,80);
        if(page_+1<pages_.size())c_.text(w-32-c_.measure(label,28),h-20,label,28,80);
        else button({w-332,h-56,300,56},label,[this]{
            for(const auto* prefix:{"reader_cre:","reader_cre:web:","reader_image:","reader_image:web:"})
                store_.set_setting(std::string(prefix)+std::to_string(current_->id),"");
            store_.finish_reading(current_->id);
            auto it=std::find_if(list_.begin(),list_.end(),[this](const Entry& e){return e.id==current_->id;});
            if(it!=list_.end()&&++it!=list_.end())open(it->id);
            else{cre_.reset();current_.reset();pages_.clear();notify("本次列表已读完");}
        },false,28,Icon::Next,true,Align::Right);
    }
    const int content_bottom=reader_panel_==ReaderPanel::None?(reader_tools_?h-reader_bar_height:h-56):cover_top;
    hits_.push_back({{0,reader_content_top,w,content_bottom-reader_content_top},[this,w]{
        if(reader_panel_!=ReaderPanel::None){close_reader_tools();return;}
        const int x=last_tap_x_;
        if(reader_tools_){close_reader_tools();return;}
        if(x<w/4)previous();else if(x>=3*w/4)next();else{reader_tools_=true;}
    }});
    if(!cre_&&reader_panel_==ReaderPanel::None&&!reader_tools_){
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
    const int w=c_.width,h=c_.height,y=h-reader_bar_height;
    c_.fill({0,y,w,reader_bar_height},255);c_.rule(32,y,w-64,170);
    const std::array<std::tuple<const char*,Icon,ReaderPanel>,4> tools{{
        {"排版",Icon::None,ReaderPanel::Type},{"正文",Icon::Download,ReaderPanel::Content},{"图片",Icon::Image,ReaderPanel::Images},{"信息",Icon::Info,ReaderPanel::Info}}};
    for(std::size_t i=0;i<tools.size();++i){
        const int tw=(w-64)/4,x=32+static_cast<int>(i)*tw;
        const auto label=std::get<0>(tools[i]);const auto icon=std::get<1>(tools[i]);const auto panel=std::get<2>(tools[i]);
        const int left=x+(tw-108)/2;
        if(icon==Icon::None)c_.text(left,y+60,"Aa",32);
        else c_.icon(icon,{left,y+32,32,32});
        c_.text(left+48,y+60,label,28);
        if(reader_panel_==panel)c_.fill({x,y+reader_bar_height-4,tw,4},30);
        hits_.push_back({{x,y,tw,reader_bar_height},[this,panel]{
            typography_anchor_.reset();
            if(reader_panel_==panel){reader_panel_=ReaderPanel::None;image_zoom_=false;}
            else{reader_panel_=panel;image_zoom_=false;info_scroll_=0;}
        }});
    }
}
int App::reader_panel_height()const{
    if(reader_panel_==ReaderPanel::None)return 0;
    if(reader_panel_==ReaderPanel::Type)return 640;
    if(reader_panel_==ReaderPanel::Images)return image_zoom_?764:662;
    return reader_panel_==ReaderPanel::Info?600:560;
}
void App::reader_panel_screen(){
    const int w=c_.width,h=c_.height,bar=reader_bar_height;
    const int height=reader_panel_height();
    const int y=h-bar-height;
    c_.fill({0,y,w,height},255);c_.rule(32,y,w-64,150);
    const auto close=[this]{reader_panel_=ReaderPanel::None;image_zoom_=false;typography_anchor_.reset();};
    const char* title=reader_panel_==ReaderPanel::Type?"排版":reader_panel_==ReaderPanel::Images?"图片":reader_panel_==ReaderPanel::Content?"正文来源":"文章信息";
    c_.text(32,y+54,title,32);
    button({w-184,y,152,88},"收起",close,false,28,Icon::Close,true,Align::Right);
    auto adjust=[this](int& value,int next,const std::string& key){
        if(value==next)return;
        // Keep one text/image anchor across repeated live adjustments instead of drifting to each new page start.
        if(!typography_anchor_){save_position();typography_anchor_=current_->position;}
        current_->position=*typography_anchor_;
        value=next;store_.set_setting(key,std::to_string(value));paginate();
    };
    if(reader_panel_==ReaderPanel::Type){
        const int start=w-496,cell=144,gap=16;
        const auto choice=[this](Rect r,const std::string& label,std::function<void()> action,bool selected){
            frame(c_,r,selected?3:1);button(r,label,std::move(action),selected,28);
        };
        c_.text(32,y+150,"字号",32);
        const Rect smaller{start,y+100,cell,88},larger{start+2*(cell+gap),y+100,cell,88};
        frame(c_,smaller);frame(c_,larger);
        button(smaller,"−",[this,adjust]{adjust(font_size_,std::max(24,font_size_-2),"font_size");},false,36,Icon::None,font_size_>24);
        const auto value=std::to_string(font_size_);
        c_.text(start+cell+gap+(cell-c_.measure(value,32))/2,y+150,value,32);
        button(larger,"+",[this,adjust]{adjust(font_size_,std::min(48,font_size_+2),"font_size");},false,36,Icon::None,font_size_<48);
        const std::array<int,3> gaps{16,24,32};
        const std::array<int,4> margins{32,64,78,96};
        const std::array<const char*,3> lines{"紧凑","标准","宽松"};
        const std::array<const char*,4> widths{"窄","适中","标准","宽"};
        c_.text(32,y+256,"行距  "+std::to_string(line_height()),32);
        c_.text(32,y+360,"页边距  "+std::to_string(margin_),32);
        for(int i=0;i<3;++i)choice({start+i*(cell+gap),y+206,cell,88},lines[i],
            [this,adjust,gaps,i]{adjust(line_gap_,gaps[i],"line_gap");},line_gap_==gaps[i]);
        for(int i=0;i<4;++i)choice({start+i*(104+gap),y+310,104,88},widths[i],
            [this,adjust,margins,i]{adjust(margin_,margins[i],"margin");},margin_==margins[i]);
        if(CreReader::available()){
            c_.text(32,y+456,"排版引擎",32);
            choice({start,y+406,224,88},"原排版",[this]{set_reader_engine(false);},!cre_);
            choice({start+240,y+406,224,88},"新版排版",[this]{set_reader_engine(true);},static_cast<bool>(cre_));
        }
        c_.text(32,y+560,"调整后会保留当前阅读位置",24,85);
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
            c_.text(32,y+area_height+118,c_.ellipsize(caption,24,w-64),24,90);
            const int controls=h-bar-100;
            button({32,controls,216,88},"上一张",[this]{previous();},false,28,Icon::Back,image_page_>0,Align::Left);
            const auto counter=std::to_string(image_page_+1)+" / "+std::to_string(current_->images.size());
            c_.text(w/2-180+(160-c_.measure(counter,24))/2,controls+52,counter,24,90);
            button({w/2+12,controls,176,88},image_zoom_?"还原":"放大",[this]{image_zoom_=!image_zoom_;},false,28,Icon::None,shown);
            button({w-248,controls,216,88},"下一张",[this]{next();},false,28,Icon::Next,image_page_+1<current_->images.size(),Align::Right);
        }
    }else if(reader_panel_==ReaderPanel::Content){
        const auto state=store_.fulltext_status(current_->id);
        c_.text(32,y+130,current_->web_content?"当前：网页正文":"当前：RSS 原文",28);
        button({32,y+166,(w-80)/2,88},"RSS 原文",[this]{set_reader_content(false);},!current_->web_content,28);
        button({w/2+8,y+166,(w-80)/2,88},"网页正文",[this]{set_reader_content(true);},current_->web_content,28,Icon::None,state.state=="ready");
        std::string message;
        if(state.state=="ready"){
            int missing=0;if(auto web=store_.fulltext_entry(current_->id))for(const auto& image:web->images)if(!image.width)++missing;
            message=missing?"正文已保存 · "+std::to_string(missing)+" 张图片未缓存，返回列表同步可重试":"网页正文和所含缓存图片可离线阅读";
        }else if(state.state=="pending")message=sync_state_==SyncState::Running?"正在下载，当前阅读内容不会替换":"等待下载；可继续下载或返回列表同步";
        else if(state.state=="failed")message="下载失败："+state.error;
        else message="尝试从原网页提取正文；失败仍保留 RSS 原文";
        int line_y=y+300;for(const auto& line:c_.wrap(message,28,w-64)){if(line_y>y+height-120)break;c_.text(32,line_y,line.text,28,85);line_y+=42;}
        const bool running=sync_state_==SyncState::Running;
        button({32,y+height-100,w-64,88},running?"取消下载":state.state=="ready"?"网页正文已保存":state.state=="failed"?"重试下载网页正文":state.state=="pending"?"继续下载网页正文":"下载网页正文",[this,running]{
            if(running){request_="cancel-sync";return;}
            save_position();store_.queue_fulltext(current_->id);
            if(store_.fulltext_status(current_->id).state!="pending"){show_error("文章地址不可下载，或订阅已取消");return;}
            request_="fulltext:"+std::to_string(current_->id);sync_started();
        },false,28,running?Icon::Close:Icon::Download,running||(state.state!="ready"&&!current_->url.empty()),Align::Left);
    }else{
        const std::string details="标题："+current_->title+"\n\n来源："+source(current_->feed_id)+"\n时间："+date(current_->published)+"\n作者："+(current_->author.empty()?"未提供":current_->author)+"\n\n原文地址：\n"+current_->url;
        const auto lines=c_.wrap(details,28,w-64);const std::size_t visible=std::max(1,(height-210)/42);
        const auto last=((lines.size()-1)/visible)*visible;info_scroll_=std::min(info_scroll_,last);
        for(std::size_t i=0;i<visible&&info_scroll_+i<lines.size();++i)c_.text(32,y+128+static_cast<int>(i)*42,lines[info_scroll_+i].text,28,30);
        if(lines.size()>visible){
            button({32,y+height-100,230,88},"上一页",[this,visible]{info_scroll_=info_scroll_>=visible?info_scroll_-visible:0;},false,28,Icon::Back,info_scroll_>0,Align::Left);
            button({w-262,y+height-100,230,88},"下一页",[this,visible,last]{info_scroll_=std::min(info_scroll_+visible,last);},false,28,Icon::Next,info_scroll_+visible<lines.size(),Align::Right);
        }
        button({w/2-210,y+height-100,420,88},current_->read?"标为未读":"标为已读",[this]{current_->read=!current_->read;store_.set_read(current_->id,current_->read);},false,28,Icon::Check);
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
    const int top=c_.height-616;
    const Rect panel{32,top,400,500};
    hits_.clear();hits_.push_back({{0,0,c_.width,c_.height},[this]{brand_menu_=false;}});
    frame(c_,panel,2);hits_.push_back({panel,[]{}});
    c_.text(56,top+58,"RSS Reader",36,30);c_.rule(56,top+80,352,205);
    button({56,top+84,352,100},"订阅",[this]{open_subscriptions();},false,32,Icon::Folder,true,Align::Left);
    c_.rule(56,top+186,352,205);
    button({56,top+188,352,100},"设置",[this]{brand_menu_=false;screen_=Screen::Settings;},false,32,Icon::Settings,true,Align::Left);
    c_.rule(56,top+290,352,205);
    button({56,top+292,352,100},"同步",[this]{brand_menu_=false;screen_=Screen::Sync;if(sync_state_!=SyncState::Running){request_="sync";sync_started();}},false,32,Icon::Sync,true,Align::Left);
    c_.rule(56,top+394,352,205);
    button({56,top+396,352,100},"退出",[this]{brand_menu_=false;confirmation_="exit";},false,32,Icon::Exit,true,Align::Left);
}
void App::settings_screen(){
    const int w=c_.width;page_header("设置");
    const bool enabled=store_.setting("home_visible")!="0";
    const auto toggle=[this,enabled]{store_.set_setting("home_visible",enabled?"0":"1");};
    button({32,270,w-316,112},"在 Kindle 首页显示 RSS订阅",toggle,false,32,Icon::None,true,Align::Left);
    button({w-252,282,220,88},enabled?"已开启":"已关闭",toggle,enabled,28);
    c_.rule(32,398,w-64,205);
    int y=468;
    for(const auto& line:c_.wrap("退出阅读器返回 Kindle 首页后生效。\n\n关闭后收起 RSS 区域，订阅和阅读记录会保留。",28,w-64)){
        c_.text(32,y,line.text,28,85);y+=44;
    }
}
void App::nav_screen(){
    const bool groups=screen_==Screen::Groups;const int w=c_.width,h=c_.height;
    page_header(groups?"管理分组":"订阅");
    button({w-212,92,180,88},groups?"新增":"添加",[this,groups]{if(groups){edit_group("",true);}else edit_feed(Feed{});},false,28,Icon::Plus,true,Align::Right);

    c_.text(32,200,std::to_string(groups?group_names_.size()-1:feeds_.size())+(groups?" 个分组":" 个订阅源"),24,85);
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
    if(starts[page]<rows.size()&&rows[starts[page]].depth>0)c_.text(32,244,c_.ellipsize(rows[starts[page]].path,24,w-64),24,100);
    for(auto i=starts[page];i<end;++i){const auto row=rows[i];const int x=40+std::min(row.depth,8)*32;
        if(!row.feed){
            c_.icon(Icon::Folder,{x,y+34,28,28});c_.text(x+48,y+62,c_.ellipsize(group_label(leaf(row.path)),32,w-x-(groups?450:310)),32);
            if(groups){
                c_.text(x+48,y+100,std::to_string(count(row.path))+" 个订阅源",24,90);
                if(!row.path.empty()){
                    button({w-352,y+10,176,88},"子分组",[this,path=row.path]{edit_group(path,true);},false,28,Icon::Plus,true,Align::Right);
                    button({w-152,y+10,120,88},"编辑",[this,path=row.path]{edit_group(path,false);},false,28,Icon::None,true,Align::Right);
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
            button({w-152,y+20,120,88},"编辑",[this,f=*f]{edit_feed(f);},false,28,Icon::None,true,Align::Right);
        }
        c_.rule(32,y+row.height-1,w-64,205);y+=row.height;
    }
    if(rows.empty())c_.text(32,350,"还没有订阅源",32,80);
    const int pager=h-(groups?100:204);c_.rule(32,pager-4,w-64,205);
    button({32,pager,220,88},"上一页",[page_ptr=&page]{--*page_ptr;},false,28,Icon::Back,page>0,Align::Left);
    const auto page_label="第 "+std::to_string(page+1)+" / "+std::to_string(starts.size())+" 页";c_.text(w/2-c_.measure(page_label,24)/2,pager+56,page_label,24,90);
    button({w-252,pager,220,88},"下一页",[page_ptr=&page]{++*page_ptr;},false,28,Icon::Next,page+1<starts.size(),Align::Right);
    if(!groups){c_.rule(32,h-104,w-64,150);button({32,h-100,w/2-64,88},"管理分组",[this]{screen_=Screen::Groups;},false,28,Icon::Folder,true,Align::Left);button({w/2,h-100,w/2-32,88},"OPML",[this]{status_.clear();screen_=Screen::Opml;},false,28,Icon::Download,true,Align::Right);}
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
        page_header(parent?"上级分组":picker_purpose_==PickerPurpose::Search?"搜索范围":"所属分组");
        button({w-212,92,180,88},"查找",[this]{start_input("group-find",picker_query_);},false,28,Icon::Search,true,Align::Right);

        c_.text(32,200,parent?"选择顶层，或放入已有分组；子分组随之移动":picker_purpose_==PickerPurpose::Move?"每个订阅源归属一个分组":"包含所选分组及其子分组",24,85);
        if(!picker_query_.empty()){c_.text(32,286,c_.ellipsize("查找："+picker_query_,28,w-260),28);button({w-212,238,180,88},"清除",[this]{picker_query_.clear();picker_page_=0;},false,28,Icon::None,true,Align::Right);top=340;}
    }
    std::vector<std::optional<std::string>> options;if(!parent&&picker_purpose_!=PickerPurpose::Move)options.push_back(std::nullopt);for(const auto& group:group_names_)if(!parent||new_group_||!under(group,editing_group_))options.push_back(group);
    std::vector<NavigationRow> rows;for(std::size_t i=0;i<options.size();++i){const auto name=options[i]?(parent&&options[i]->empty()?"顶层（无上级）":group_label(*options[i])):"全部订阅";if(lower_ascii(name).find(lower_ascii(picker_query_))!=std::string::npos)rows.push_back({name,static_cast<std::int64_t>(i+1),options[i]?static_cast<int>(std::count(options[i]->begin(),options[i]->end(),'/')):0,row_height});}
    auto starts=navigation_pages(rows,popup?6*row_height:h-104-top);picker_page_=std::min(picker_page_,starts.size()-1);nav_page_count_=starts.size();const auto begin=starts[picker_page_],end=picker_page_+1<starts.size()?starts[picker_page_+1]:rows.size();
    const int visible_rows=starts.size()>1?6:static_cast<int>(std::max<std::size_t>(1,end-begin));
    if(popup){
        const Rect panel{left,top-88,width,88+visible_rows*row_height+(starts.size()>1?88:0)+4};
        hits_.clear();hits_.push_back({{0,0,w,h},[this]{back();}});frame(c_,panel,2);hits_.push_back({panel,[]{}});
        if(picker_query_.empty())c_.text(left+24,panel.y+58,"选择分组",32);
        else button({left+24,panel.y,width-236,88},"查找："+picker_query_,[this]{start_input("group-find",picker_query_);},false,32,Icon::None,true,Align::Left);
        button({left+width-188,panel.y,164,88},picker_query_.empty()?"查找":"清除",[this]{if(picker_query_.empty())start_input("group-find",picker_query_);else{picker_query_.clear();picker_page_=0;}},false,32,picker_query_.empty()?Icon::Search:Icon::None,true,Align::Right);
        c_.rule(left+16,top-1,width-32,205);
    }
    int y=top;
    for(auto i=begin;i<end;++i){const auto row=rows[i];const auto option=options.at(row.feed-1);const int x=popup?left+24+std::min(row.depth,6)*28:40+std::min(row.depth,8)*32;
        c_.icon(Icon::Folder,{x,y+(popup?30:40),28,28});c_.text(x+48,y+(popup?58:68),c_.ellipsize(row.path,32,left+width-x-(popup?144:112)),32);
        const bool selected=parent?(option&&*option==group_parent_draft_):picker_purpose_==PickerPurpose::Move?(option&&*option==managing_->group):(!feed_&&option==group_);
        if(selected)c_.icon(Icon::Check,{left+width-(popup?56:28),y+(popup?30:40),28,28});c_.rule(popup?left+16:left,y+row_height-1,popup?width-32:width,205);
        hits_.push_back({{popup?left+2:left,y,popup?width-4:width,row_height},[this,option]{
            if(picker_purpose_==PickerPurpose::Parent){group_parent_draft_=option.value_or("");screen_=Screen::GroupEdit;}
            else if(picker_purpose_==PickerPurpose::Move){managing_->group=option.value_or("");screen_=edit_origin_;}
            else {group_=option;feed_=0;if(picker_purpose_==PickerPurpose::Search){restore_input();}else{source_list_=false;screen_=Screen::List;list_page_=0;reload();}}
        }});y+=row_height;
    }
    if(rows.empty())c_.text(popup?left+24:32,top+(popup?58:100),"没有匹配的分组",32,80);
    if(!popup||starts.size()>1){
        const int pager=popup?top+visible_rows*row_height:h-100;
        if(!popup)c_.rule(32,pager-4,w-64,205);
        button({popup?left+16:32,pager,popup?200:220,88},"上一页",[this]{--picker_page_;},false,28,Icon::Back,picker_page_>0,Align::Left);
        const auto page="第 "+std::to_string(picker_page_+1)+" / "+std::to_string(starts.size())+" 页";c_.text(left+width/2-c_.measure(page,24)/2,pager+56,page,24,90);
        button({popup?left+width-216:w-252,pager,popup?200:220,88},"下一页",[this]{++picker_page_;},false,28,Icon::Next,picker_page_+1<starts.size(),Align::Right);
    }
}
void App::edit_group(const std::string& path,bool create){
    new_group_=create;editing_group_=create?"":path;group_name_draft_=create?"":leaf(path);
    const auto slash=path.rfind('/');group_parent_draft_=create?path:slash==std::string::npos?"":path.substr(0,slash);
    screen_=Screen::GroupEdit;
}
void App::group_edit_screen(){
    const int w=c_.width,h=c_.height;page_header(new_group_?"新增分组":"编辑分组");
    c_.text(32,270,"分组名称",24,85);frame(c_,{32,296,w-64,112});
    button({56,304,w-112,96},group_name_draft_.empty()?"点击输入名称":group_name_draft_,[this]{start_input("folder",group_name_draft_);},false,32,Icon::None,true,Align::Left);
    c_.text(32,470,"上级分组",24,85);frame(c_,{32,496,w-64,112});
    button({56,504,w-168,96},group_parent_draft_.empty()?"顶层（无上级）":group_parent_draft_,[this]{open_group_picker(PickerPurpose::Parent);},false,32,Icon::Folder,true,Align::Left);
    down(c_,w-72,552);hits_.push_back({{w-112,496,80,112},[this]{open_group_picker(PickerPurpose::Parent);}});
    c_.text(32,682,new_group_?"保存在所选上级中，可继续添加更深的子分组。":"更换上级会移动整个分组，订阅和文章状态保留。",24,85);
    if(!new_group_){button({32,780,280,88},"删除分组",[this]{confirmation_="delete-group";},false,28,Icon::Close,true,Align::Left);c_.text(32,936,"删除含子分组；订阅移到未分组，文章保留。",24,85);}
    c_.rule(32,h-104,w-64,205);
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
    const int w=c_.width;page_header("OPML");
    button({32,270,w-64,112},"导入 OPML",[this]{confirmation_="import";},false,32,Icon::Download,true,Align::Left);
    c_.text(32,428,"从本机 feeds.opml 合并订阅及分组",24,85);
    button({32,510,w-64,112},"导出 OPML",[this]{request_="export";},false,32,Icon::Upload,true,Align::Left);
    c_.text(32,668,"写入 subscriptions.opml，保留空分组",24,85);
    if(!status_.empty())c_.text(32,820,c_.ellipsize(status_,24,w-64),24,75);
}
void App::sync_started(){status_.clear();sync_page_=0;sync_state_=SyncState::Running;sync_completed_=sync_failed_=0;sync_total_=static_cast<int>(feeds_.size());sync_message_="正在连接订阅源";if(screen_==Screen::Sync)dirty=true;}
void App::sync_progress(int completed,int total,int failed,const std::string& message){sync_completed_=completed;sync_total_=total;sync_failed_=failed;sync_message_=message;if(screen_==Screen::Sync)dirty=true;}
void App::sync_finished(bool cancelled,const std::string& error){
    feeds_=store_.feeds();group_names_=store_.groups();sync_failed_=std::max(sync_failed_,static_cast<int>(std::count_if(feeds_.begin(),feeds_.end(),[](const Feed& f){return !f.error.empty();})));sync_state_=cancelled?SyncState::Cancelled:(!error.empty()||sync_failed_)?SyncState::Failed:SyncState::Complete;
    if(!error.empty())sync_message_=error;
    dirty=true;
    if(screen_!=Screen::Sync)notify(reading()?sync_message_:cancelled?"同步已取消，点击更新列表":sync_state_==SyncState::Failed?"同步有未完成项目，点击更新列表":"同步已完成，点击更新列表");
}
void App::sync_screen(){
    modal_backdrop(c_);
    const int w=c_.width,h=c_.height;
    const bool running=sync_state_==SyncState::Running,failed=sync_state_==SyncState::Failed;
    const int height=failed?920:480,top=(h-height)/2,left=80,width=w-160;
    const auto heading=running?"正在同步":failed?"部分内容未能下载":sync_state_==SyncState::Cancelled?"同步已取消":sync_state_==SyncState::Complete?"同步已完成":"同步订阅";
    hits_.clear();hits_.push_back({{0,0,w,h},[]{}});
    frame(c_,{48,top,w-96,height},2);
    c_.text(left,top+64,heading,36);
    button({w-308,top+20,228,88},"缓存管理",[this]{screen_=Screen::Cache;},false,26,Icon::Settings,true,Align::Right);
    c_.text(left,top+112,"进度 "+std::to_string(sync_completed_)+" / "+std::to_string(sync_total_)+"  ·  失败 "+std::to_string(sync_failed_),24,85);
    if(failed){
        std::vector<Feed> sources;for(const auto& f:feeds_)if(!f.error.empty())sources.push_back(f);
        const std::size_t pages=std::max<std::size_t>(1,(sources.size()+3)/4);sync_page_=std::min(sync_page_,pages-1);nav_page_count_=pages;
        int y=top+136;
        for(std::size_t i=sync_page_*4;i<std::min(sources.size(),(sync_page_+1)*4);++i){const auto f=sources[i];
            button({left,y,width,72},f.title.empty()?f.url:f.title,[this,f]{edit_feed(f);},false,30,Icon::Rss,true,Align::Left);
            c_.text(left,y+100,c_.ellipsize(f.error,24,width),24,90);y+=130;
        }
        if(sources.empty()&&!sync_message_.empty())for(const auto& line:c_.wrap(sync_message_,28,width)){if(y>top+height-260)break;c_.text(left,y+32,line.text,28,85);y+=42;}
        if(pages>1){
            button({left,top+height-204,220,88},"上一页",[this]{--sync_page_;},false,28,Icon::Back,sync_page_>0,Align::Left);
            const auto label=std::to_string(sync_page_+1)+" / "+std::to_string(pages);c_.text(w/2-c_.measure(label,24)/2,top+height-148,label,24,90);
            button({w-left-220,top+height-204,220,88},"下一页",[this]{++sync_page_;},false,28,Icon::Next,sync_page_+1<pages,Align::Right);
        }
    }else{
        frame(c_,{left,top+144,width,22},2);
        if(sync_total_>0)c_.fill({left+2,top+146,static_cast<int>(static_cast<long long>(width-4)*std::clamp(sync_completed_,0,sync_total_)/sync_total_),18},30);
        int y=top+218;
        for(const auto& line:c_.wrap(sync_message_,28,width)){if(y>top+300)break;c_.text(left,y,line.text,28,85);y+=40;}
    }
    if(!status_.empty())c_.text(left,top+height-(failed?236:144),c_.ellipsize(status_,24,width),24,80);
    const int actions=top+height-104;c_.rule(left,actions,width,205);
    button({left,actions+4,400,88},running?"取消同步":failed?"重试失败项目":"重新同步",[this,running,failed]{if(running)request_="cancel-sync";else{request_=failed?"retry-sync":"sync";sync_started();}},false,28,running?Icon::Close:Icon::Sync,true,Align::Left);
    button({w-left-280,actions+4,280,88},running?"后台运行":"关闭",[this]{back();},true,28);
}
void App::input_screen(){
    const int w=c_.width,h=c_.height;
    const auto title=input_mode_=="url"?"订阅地址":input_mode_=="title"?"订阅名称":input_mode_=="folder"?"分组名称":input_mode_=="group-find"?"查找分组":"搜索文章";
    std::string hint=input_mode_=="url"?"输入 HTTP(S) RSS / Atom 地址":input_mode_=="title"?"可留空，同步时自动读取博客名称":input_mode_=="folder"?"请输入不含 / 的分组名称":input_mode_=="group-find"?"按分组名称或路径查找":"搜索本地文章标题";
    if(input_mode_=="search")hint+=" · 范围："+(feed_?source(feed_):group_?(group_->empty()?"未分组":*group_):"全部订阅");
    page_header(title);
    const int hint_size=28;
    c_.text(32,200,c_.ellipsize(hint,hint_size,w-64),hint_size,75);
    if(input_mode_=="search")hits_.push_back({{32,166,w-64,88},[this]{open_group_picker(PickerPurpose::Search);}});
    c_.rule(32,230,w-64,205);
    const auto frame=[this](Rect r){c_.fill(r,150);c_.fill({r.x+1,r.y+1,r.w-2,r.h-2},255);};
    frame({32,254,w-64,200});
    auto lines=c_.wrap(input_,32,w-112);if(lines.empty())lines.push_back(Line{});
    const auto first=lines.size()>4?lines.size()-4:0;
    for(std::size_t i=first;i<lines.size();++i)c_.text(56,296+static_cast<int>(i-first)*44,lines[i].text,32);
    const int cursor_x=56+c_.measure(lines.back().text,32);
    c_.fill({cursor_x,273+static_cast<int>(lines.size()-first-1)*44,2,32},30);
    c_.text(32,506,input_mode_=="url"?"支持 RSS / Atom，也可通过 OPML 批量导入。":"点击确定应用输入；返回保留原内容。",24,75);
    c_.rule(32,534,w-64,205);
    c_.text(32,578,chinese_?(composing_.empty()?"中文拼音 · ü 用 v":"中文拼音 · "+composing_):symbols_?"英文 · 符号":"英文 · 字母",28,30);
    const auto candidates=chinese_?pinyin_candidates(composing_):std::vector<std::string>{};
    const auto pages=(candidates.size()+5)/6;
    if(candidate_page_>=std::max<std::size_t>(1,pages))candidate_page_=0;
    for(std::size_t i=candidate_page_*6;i<std::min(candidates.size(),(candidate_page_+1)*6);++i){
        const auto slot=i%6;const auto word=candidates[i];const int candidate_width=(w-88)/3;
        const Rect candidate{32+static_cast<int>(slot%3)*(candidate_width+12),598+static_cast<int>(slot/3)*100,candidate_width,88};
        frame(candidate);button(candidate,word,[this,word]{text_input(word);},false,32);
    }
    if(chinese_&&candidates.empty())c_.text(32,660,composing_.empty()?"输入拼音后，点击候选词上屏。":"暂无候选，请修改拼音。",24,100);
    if(pages>1){
        button({32,806,220,88},"上一组",[this]{--candidate_page_;},false,28,Icon::Back,candidate_page_>0,Align::Left);
        const auto page=std::to_string(candidate_page_+1)+" / "+std::to_string(pages);c_.text((w-c_.measure(page,24))/2,860,page,24,100);
        button({w-252,806,220,88},"下一组",[this]{++candidate_page_;},false,28,Icon::Next,candidate_page_+1<pages,Align::Right);
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
            frame(key);button(key,ch,[this,ch]{keyboard_key(ch);},false,32);
        }
        y+=key_height+key_gap;
    }
    const Rect shift{32,top+2*(key_height+key_gap),144,key_height},erase{w-176,shift.y,144,key_height};
    frame(shift);button(shift,chinese_?"清拼音":shift_?"小写":"大写",[this]{if(chinese_){composing_.clear();candidate_page_=0;}else shift_=!shift_;},false,28,Icon::None,chinese_?!composing_.empty():!symbols_);
    frame(erase);button(erase,"删除",[this]{backspace();},false,28);
    const Rect symbols{32,y,144,key_height},space{192,y,420,key_height},language{628,y,168,key_height},confirm{812,y,w-844,key_height};
    frame(symbols);button(symbols,symbols_?"字母":"符号",[this]{symbols_=!symbols_;},false,28);
    frame(space);button(space,composing_.empty()?"空格":"选首词",[this]{input_space();},false,28);
    frame(language);button(language,input_mode_=="url"?"英文":chinese_?"切换英文":"切换中文",[this]{
        if(!composing_.empty()){const auto raw=composing_;text_input(raw);if(!composing_.empty())return;}
        chinese_=!chinese_;shift_=false;
    },false,28,Icon::None,input_mode_!="url");
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
    auto_fulltext_=feed.id&&store_.setting("feed_fulltext:"+std::to_string(feed.id))=="1";
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
        draft.id=store_.add_feed(draft);
    }
    store_.set_setting("feed_fulltext:"+std::to_string(draft.id),auto_fulltext_?"1":"0");
    managing_.reset();screen_=edit_origin_;reload();notify("订阅更改已保存");
}
void App::management_screen(){
    const int w=c_.width,h=c_.height;const bool storage=screen_==Screen::Cache&&!managing_;
    page_header(storage?"缓存管理":managing_->id?"编辑订阅":"添加订阅");
    if(storage){
        const auto entries=store_.entries(View::All);const auto cutoff=static_cast<std::int64_t>(std::time(nullptr))-30LL*86400;
        const auto starred=std::count_if(entries.begin(),entries.end(),[](const Entry& e){return e.starred;});
        const auto eligible=std::count_if(entries.begin(),entries.end(),[&](const Entry& e){return e.read&&!e.starred&&!e.in_progress&&e.published<cutoff;});
        c_.text(32,300,"本地文章  "+std::to_string(entries.size())+" 篇",32);
        c_.text(32,380,"已收藏    "+std::to_string(starred)+" 篇",32);
        c_.text(32,460,"可清理    "+std::to_string(eligible)+" 篇",32);
        c_.rule(32,520,w-64,205);c_.text(32,592,"清理 30 天前的已读文章",32);
        int y=680;for(const auto& line:c_.wrap("收藏、未读与正在阅读的文章会保留。\n\n清理会删除这些文章的本地内容和状态；订阅源仍提供的文章可能在同步后重新出现。",28,w-64)){c_.text(32,y,line.text,28,85);y+=44;}
        c_.rule(32,h-104,w-64,205);button({32,h-100,440,88},"清理旧已读缓存",[this]{confirmation_="clean";},false,28,Icon::Download,true,Align::Left);return;
    }
    c_.text(32,254,"名称（选填）",24,85);frame(c_,{32,280,w-64,112});
    button({56,288,w-112,96},managing_->title.empty()?"留空，同步时自动读取博客名称":managing_->title,[this]{start_input("title",managing_->title);},false,32,Icon::None,true,Align::Left);
    c_.text(32,442,"RSS 地址",24,85);frame(c_,{32,466,w-64,160});auto url=c_.wrap(managing_->url,32,w-112);
    for(std::size_t i=0;i<std::min<std::size_t>(3,url.size());++i)c_.text(56,508+static_cast<int>(i)*38,c_.ellipsize(url[i].text+(i==2&&url.size()>3?"…":""),32,w-112),32);
    hits_.push_back({{32,466,w-64,160},[this]{start_input("url",managing_->url);}});
    c_.text(32,700,"所属分组",24,85);frame(c_,{32,724,w-64,96});
    button({56,728,w-112,88},group_label(managing_->group),[this]{open_group_picker(PickerPurpose::Move);},false,32,Icon::Folder,true,Align::Left);
    if(managing_->id){
        button({32,1080,320,88},"取消订阅",[this]{confirmation_="remove";},false,28,Icon::Close,true,Align::Left);
        button({w-452,1080,420,88},"本订阅标为已读",[this]{confirmation_="read";},false,28,Icon::Check,true,Align::Right);
        c_.text(32,1220,"取消订阅仅保留收藏；上述操作均需确认。",24,85);
    }
    button({32,876,w-64,88},std::string(auto_fulltext_?"✓ ":"○ ")+"自动下载网页正文",[this]{auto_fulltext_=!auto_fulltext_;},false,28,Icon::None,true,Align::Left);
    c_.text(32,1010,"仅下载之后新增的文章；失败保留 RSS 原文。",24,85);
    if(!status_.empty())c_.text(32,h-152,c_.ellipsize(status_,24,w-64),24,80);
    c_.rule(32,h-104,w-64,205);
    button({32,h-100,320,88},"放弃更改",[this]{managing_.reset();screen_=edit_origin_;},false,28,Icon::None,true,Align::Left);
    button({w-312,h-100,280,88},"保存更改",[this]{save_feed();},true,28);
}
void App::confirmation_screen(){
    const int w=c_.width,h=c_.height;
    if(confirmation_=="exit"){
        list_screen();hits_.clear();hits_.push_back({{0,0,w,h},[this]{confirmation_.clear();}});
        modal_backdrop(c_);
        const Rect dialog{48,(h-352)/2,w-96,352};frame(c_,dialog,2);
        hits_.push_back({dialog,[]{}});
        c_.text(dialog.x+32,dialog.y+64,"退出 RSS Reader？",36);
        c_.text(dialog.x+32,dialog.y+132,"订阅、收藏与阅读位置都会保留。",28,85);
        c_.rule(dialog.x+32,dialog.y+248,dialog.w-64,205);
        button({dialog.x+32,dialog.y+252,320,88},"取消",[this]{confirmation_.clear();},false,28,Icon::None,true,Align::Left);
        button({dialog.x+dialog.w-312,dialog.y+252,280,88},"退出应用",[this]{confirmation_.clear();quit=true;},true,28);return;
    }
    page_header(confirmation_=="remove"?"取消订阅":confirmation_=="read"?"标为已读":confirmation_=="delete-group"?"删除分组":confirmation_=="import"?"导入 OPML":"清理缓存");
    const auto message=confirmation_=="remove"?"取消这个订阅？\n\n收藏文章会保留。其余文章及阅读进度将从本机删除。":
        confirmation_=="read"?"将这个订阅的全部文章标为已读？\n\n收藏和阅读位置会保留。":
        confirmation_=="delete-group"?"删除这个分组？\n\n此分组及子分组会移除，组内订阅移到未分组，所有文章和状态保留。":
        confirmation_=="import"?"导入本地 OPML？\n\n从 feeds.opml 合并订阅与分组；重复地址不会再次添加。":
        "清理 30 天前的已读文章？\n\n收藏、未读和正在阅读的文章会保留。此操作无法撤销。";
    int y=250;for(const auto& line:c_.wrap(message,32,w-64)){c_.text(32,y,line.text,32);y+=50;}
    c_.rule(32,h-104,w-64,205);
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
void App::touch(const TouchGesture& gesture){
    const auto& low=gesture.minimum;const auto& high=gesture.maximum;
    const auto inside=[&](TouchPoint p){return p.x>=low.x&&p.x<=high.x&&p.y>=low.y&&p.y<=high.y;};
    if(low.x<0||low.y<0||high.x>=c_.width||high.y>=c_.height||
       low.x>high.x||low.y>high.y||!inside(gesture.start)||!inside(gesture.end))return;
    // A small amount of finger jitter remains a tap. Other drags must never activate the release target.
    if(high.x-low.x<=24&&high.y-low.y<=24){tap(gesture.end.x,gesture.end.y);return;}
    if(!current_||reader_tools_||reader_panel_!=ReaderPanel::None||!error_.empty()||
       !confirmation_.empty()||!input_mode_.empty()||managing_||brand_menu_||
       screen_==Screen::GroupPicker||screen_==Screen::GroupEdit)return;
    if(low.y<reader_content_top||high.y>=c_.height-56)return;
    const int dx=gesture.end.x-gesture.start.x,dy=gesture.end.y-gesture.start.y;
    if(std::abs(dx)<80||2*std::abs(dx)<3*std::abs(dy))return;
    if(dx<0&&page_+1<pages_.size())next();
    else if(dx>0&&page_>0)previous();
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
    else if(current_){
        bool saved=true;
        try{save_position();}catch(const std::exception& e){
            saved=false;std::fprintf(stderr,"RSS Reader: unable to save position on exit: %s\n",e.what());
        }
        cre_.reset();current_.reset();pages_.clear();
        if(!saved)notify("阅读进度未保存，请检查剩余空间或稍后重试");
    }
    else if(screen_==Screen::Subscriptions)restore_home();
    else if(screen_==Screen::Groups||screen_==Screen::Opml)screen_=Screen::Subscriptions;
    else if(screen_==Screen::Cache)screen_=Screen::Sync;
    else if(screen_==Screen::Settings)screen_=Screen::List;
    else if(screen_==Screen::Sync){screen_=Screen::List;if(sync_state_!=SyncState::Running){status_.clear();reload();}}
    else if(source_list_)screen_=Screen::Subscriptions;
    dirty=true;
}
void App::set_reader_status(const ReaderStatus& status){
    if(reader_status_==status)return;
    reader_status_=status;dirty=true;
}
void App::notify(const std::string& text){status_=text;list_starts_={0};list_complete_=false;ensure_list_page(list_page_);list_page_=std::min(list_page_,list_starts_.size()-1);dirty=true;}
void App::show_error(const std::string& text){error_=text;dirty=true;}
std::string App::take_request(){auto request=request_;request_.clear();return request;}
}
