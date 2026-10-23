#include "app.hpp"
#include "pinyin.hpp"
#include "feed.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <set>

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
int read_int(Store& store,const std::string& key,int fallback,int low,int high){
    try{return std::clamp(std::stoi(store.setting(key)),low,high);}catch(...){return fallback;}
}
}
App::App(Store& store,Canvas& canvas):store_(store),c_(canvas){
    font_size_=read_int(store_,"font_size",32,24,48);
    line_gap_=read_int(store_,"line_gap",20,10,30);
    margin_=read_int(store_,"margin",64,40,100);
    reload();
}
int App::row_height(const Entry& e){
    return static_cast<int>(std::min<std::size_t>(3,c_.wrap(e.title,36,c_.width-158).size()))*46+94;
}
void App::ensure_list_page(std::size_t page){
    while(!list_complete_&&list_starts_.size()<=page){
        const auto start=list_starts_.back();int y=280;std::size_t i=start;
        for(;i<list_.size();++i){
            const int height=row_height(list_[i]);
            if(y+height>c_.height-180&&i>start)break;
            y+=height;
        }
        if(i==list_.size())list_complete_=true;
        else list_starts_.push_back(i);
    }
}
// shortcut: list metadata still loads in full; use database paging if very large libraries make queries slow.
void App::reload(){
    feeds_=store_.feeds();list_=store_.entries(view_,feed_,search_,midnight(),group_);
    list_starts_={0};list_complete_=false;
    ensure_list_page(list_page_);
    list_page_=std::min(list_page_,list_starts_.size()-1);dirty=true;
}
void App::show_view(View v,std::int64_t feed){
    view_=v;feed_=feed;group_.reset();search_.clear();list_page_=0;navigation_=false;reload();
}
void App::show_group(const std::string& group){
    view_=View::All;feed_=0;group_=group;search_.clear();list_page_=0;navigation_=false;reload();
}
std::string App::source(std::int64_t id)const{
    for(const auto& f:feeds_)if(f.id==id)return f.title.empty()?f.url:f.title;
    return "已保留的收藏";
}
std::string App::view_name()const{
    if(!search_.empty())return "搜索结果";
    if(feed_)return source(feed_);
    if(group_)return group_->empty()?"未分组":*group_;
    switch(view_){case View::Unread:return "未读文章";case View::All:return "全部文章";
        case View::Starred:return "我的收藏";case View::Today:return "今日更新";}
    return "文章";
}
void App::button(Rect r,const std::string& label,std::function<void()> action,bool filled,int size,Icon icon){
    if(filled)c_.fill(r,25);
    const int symbol=icon==Icon::None?0:28,gap=symbol?12:0;
    const auto text=c_.ellipsize(label,size,r.w-20-symbol-gap);
    const int left=r.x+(r.w-c_.measure(text,size)-symbol-gap)/2;
    if(symbol)c_.icon(icon,{left,r.y+(r.h-symbol)/2,symbol,symbol},filled?255:30);
    c_.text(left+symbol+gap,r.y+(r.h+size)/2-5,text,size,filled?255:30);
    hits_.push_back({r,std::move(action)});
}
void App::draw(){
    hits_.clear();c_.clear();
    if(!error_.empty()){
        c_.text(56,100,"操作未完成",40);int y=220;
        for(const auto& line:c_.wrap(error_,28,c_.width-112)){if(y>c_.height-220)break;c_.text(56,y,line.text,28);y+=44;}
        button({56,c_.height-150,c_.width-112,84},"返回",[this]{error_.clear();},true);
    }
    else if(!confirmation_.empty())confirmation_screen();
    else if(!input_mode_.empty())input_screen();
    else if(managing_||storage_)management_screen();
    else if(navigation_)nav_screen();
    else if(current_){if(images_)image_screen();else if(info_)info_screen();else{reader_screen();if(menu_)reader_menu();}}
    else list_screen();
    dirty=false;
}
void App::list_screen(){
    ensure_list_page(list_page_+1);
    const int w=c_.width,h=c_.height,m=64;
    c_.icon(Icon::Rss,{m,43,34,34});
    c_.text(m+50,76,"RSS Reader",24,30);
    button({w-190,32,140,66},"同步",[this]{request_="sync";},false,26,Icon::Sync);
    const auto heading=c_.ellipsize(view_name(),48,w-330);
    c_.text(m,178,heading,48);
    const int arrow_x=m+c_.measure(heading,48)+24;
    for(int i=0;i<10;++i)c_.fill({arrow_x+i,151+i,20-2*i,1},30);
    hits_.push_back({{40,107,w-270,90},[this]{navigation_=true;nav_page_=0;}});
    button({w-190,118,140,66},"搜索",[this]{start_input("search",search_);},false,26,Icon::Search);
    c_.text(m,231,std::to_string(list_.size())+" 篇文章",24,75);
    try {
        const auto last=std::stoll(store_.setting("last_entry","0"));
        if(last&&store_.entry(last,false))button({w-280,193,225,55},"继续阅读 →",[this,last]{open(last);},false,23);
    }catch(const std::invalid_argument&){}catch(const std::out_of_range&){}
    c_.rule(m,254,w-2*m,180);
    if(list_.empty()){
        c_.text(m,h/2-80,feeds_.empty()?"把喜欢的内容，留在这里。":"暂时没有文章",36);
        c_.text(m,h/2-20,feeds_.empty()?"导入 OPML 订阅列表，或添加一个订阅地址。":"同步订阅，或切换到全部文章继续阅读。",25,90);
        if(feeds_.empty()){
            button({m,h/2+40,230,76},"导入订阅",[this]{request_="import";},true);
            button({m+270,h/2+40,230,76},"添加订阅",[this]{start_input("add","https://");},false,26,Icon::Plus);
        }else button({m,h/2+40,230,76},"全部文章",[this]{show_view(View::All);},true);
    }
    int y=280;
    const auto end=list_page_+1<list_starts_.size()?list_starts_[list_page_+1]:list_.size();
    for(auto i=list_starts_[list_page_];i<end;++i){
        auto& e=list_[i];const int height=row_height(e);
        auto fresh=store_.entry(e.id,false);if(fresh){e.read=fresh->read;e.starred=fresh->starred;}
        const auto lines=c_.wrap(e.title,36,w-2*m-30);
        const auto count=std::min<std::size_t>(3,lines.size());
        for(std::size_t n=0;n<count;++n){
            auto title=lines[n].text;
            if(n==2&&lines.size()>3)title=c_.ellipsize(title+"…",36,w-2*m-30);
            c_.text(m,y+40+static_cast<int>(n)*46,title,36,e.read?90:20);
        }
        if(e.starred)c_.icon(Icon::Star,{w-m-28,y,26,26});
        auto meta=source(e.feed_id)+"  ·  "+date(e.published)+(e.read?"  ·  已读":"");
        c_.text(m,y+static_cast<int>(count)*46+30,c_.ellipsize(meta,24,w-2*m-40),24,75);
        c_.rule(m,y+height-14,w-2*m,205);
        hits_.push_back({{m,y,w-2*m,height-10},[this,id=e.id]{open(id);}});
        y+=height;
    }
    if(!status_.empty())button({m,h-148,w-2*m,58},status_,[this]{reload();status_.clear();},false,22);
    c_.rule(m,h-100,w-2*m,210);
    const auto count=list_starts_.size();
    button({36,h-84,180,68},"上一页",[this]{previous();},false,26,Icon::Back);
    c_.text(w/2-55,h-39,std::to_string(list_page_+1)+" / "+(list_complete_?std::to_string(count):"…"),23,90);
    button({w-216,h-84,180,68},"下一页",[this]{next();},false,26,Icon::Next);
}
int App::article_heading_height(){
    return 54*static_cast<int>(std::min<std::size_t>(3,c_.wrap(current_->title,42,c_.width-2*margin_).size()))+60;
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
    pages_=c_.paginate_article(current_->text,inline_images,font_size_,line_height(),c_.width-2*margin_,c_.height-245,
                               c_.height-245-article_heading_height());
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
    menu_=false;info_=false;images_=false;image_page_=0;dirty=true;
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
    c_.text(margin_,65,c_.ellipsize(source(current_->feed_id),23,w-2*margin_-230),23,100);
    hits_.push_back({{margin_,10,w-2*margin_-230,80},[this]{info_=true;}});
    if(!current_->images.empty())button({w-margin_-210,18,210,70},"图片 · "+std::to_string(current_->images.size()),[this]{images_=true;},false,23,Icon::Image);
    else if(current_->starred)c_.icon(Icon::Star,{w-margin_-28,36,28,28});
    c_.rule(margin_,97,w-2*margin_);
    const int top=125+(page_==0?article_heading_height():0);
    if(page_==0){
        const auto title=c_.wrap(current_->title,42,w-2*margin_);
        for(std::size_t i=0;i<std::min<std::size_t>(3,title.size());++i)
            c_.text(margin_,163+static_cast<int>(i)*54,i==2&&title.size()>3?c_.ellipsize(title[i].text+"…",42,w-2*margin_):title[i].text,42,15);
    }
    for(const auto& item:pages_[page_].elements){
        const Rect area{margin_+item.rect.x,top+item.rect.y,item.rect.w,item.rect.h};
        if(item.image>=0){
            auto cached=store_.image(current_->id,current_->images.at(item.image).url);
            if(!cached||!draw_image(*cached,area)){
                c_.fill(area,245);
                c_.text(area.x+20,area.y+area.h/2+10,c_.ellipsize("图片未缓存 · 点击查看",24,area.w-40),24,95);
            }
        }else c_.text(area.x+(item.caption?(area.w-c_.measure(item.line.text,item.size))/2:0),
                      area.y+item.size,item.line.text,item.size,item.caption?90:20);
    }
    c_.rule(margin_,h-103,w-2*margin_,200);
    c_.fill({margin_,h-104,static_cast<int>((w-2*margin_)*(page_+1)/pages_.size()),3},30);
    const auto footer=std::to_string(page_+1)+" / "+std::to_string(pages_.size());
    c_.text((w-c_.measure(footer,22))/2,h-49,footer,22,100);
    hits_.push_back({{0,100,w/3,h-210},[this]{previous();}});
    hits_.push_back({{2*w/3,100,w-w*2/3,h-210},[this]{next();}});
    hits_.push_back({{w/3,100,w/3,h-210},[this]{menu_=true;}});
    for(const auto& item:pages_[page_].elements)if(item.image>=0)
        hits_.push_back({{margin_+item.rect.x,top+item.rect.y,item.rect.w,item.rect.h},[this,index=item.image]{image_page_=index;images_=true;}});
    button({22,h-90,170,70},"返回列表",[this]{back();},false,23,Icon::Back);
    if(page_+1==pages_.size())button({w-240,h-90,210,70},"下一篇 →",[this]{
        store_.finish_reading(current_->id);
        auto it=std::find_if(list_.begin(),list_.end(),[this](const Entry& e){return e.id==current_->id;});
        if(it!=list_.end()&&++it!=list_.end())open(it->id);
        else{current_.reset();pages_.clear();notify("本次列表已读完");}
    },false,23);
    else button({w-200,h-90,180,70},"阅读设置",[this]{menu_=true;},false,23,Icon::Settings);
}
void App::reader_menu(){
    hits_.clear();const int w=c_.width,h=c_.height,top=h-610;
    c_.fill({0,top,w,610},255);c_.rule(0,top,w,20);
    c_.text(64,top+65,"阅读设置",34);
    c_.rule(64,top+184,w-128,180);
    for(int y:{190,284,376}){
        c_.fill({w-360,top+y,310,74},205);
        c_.fill({w-358,top+y+2,306,70},255);
    }
    if(!current_->images.empty())button({w/2-140,top+18,280,70},"图片 "+std::to_string(current_->images.size()),[this]{images_=true;menu_=false;},false,26,Icon::Image);
    button({w-170,top+18,125,70},"收起",[this]{menu_=false;});
    button({40,top+100,w/2-55,76},current_->starred?"取消收藏":"收藏文章",[this]{
        current_->starred=!current_->starred;store_.set_starred(current_->id,current_->starred);menu_=false;
    },false,26,Icon::Star);
    button({w/2+10,top+100,w/2-55,76},current_->read?"标为未读":"标为已读",[this]{
        current_->read=!current_->read;store_.set_read(current_->id,current_->read);menu_=false;
    },false,26,Icon::Check);
    auto adjust=[this](int& value,int delta,int low,int high,const std::string& key){
        save_position();value=std::clamp(value+delta,low,high);store_.set_setting(key,std::to_string(value));paginate();
    };
    c_.text(64,top+238,"字号",28);
    button({w-360,top+190,130,74},"−",[this,adjust]{adjust(font_size_,-2,24,48,"font_size");});
    c_.text(w-216,top+239,std::to_string(font_size_),27);
    button({w-140,top+190,95,74},"+",[this,adjust]{adjust(font_size_,2,24,48,"font_size");});
    c_.text(64,top+330,"行距",28);
    button({w-350,top+284,140,74},"紧凑",[this,adjust]{adjust(line_gap_,-4,10,30,"line_gap");});
    button({w-190,top+284,140,74},"宽松",[this,adjust]{adjust(line_gap_,4,10,30,"line_gap");});
    c_.text(64,top+422,"页边距",28);
    button({w-350,top+376,140,74},"减小",[this,adjust]{adjust(margin_,-12,40,100,"margin");});
    button({w-190,top+376,140,74},"增大",[this,adjust]{adjust(margin_,12,40,100,"margin");});
    button({40,top+490,w-80,76},"返回文章列表",[this]{menu_=false;back();},false,26,Icon::Back);
}
void App::nav_screen(){
    const int w=c_.width,h=c_.height;
    c_.text(64,88,"我的阅读",44);
    button({w-190,30,140,76},"关闭",[this]{navigation_=false;},false,26,Icon::Close);
    const std::vector<std::pair<std::string,View>> views={{"未读文章",View::Unread},{"全部文章",View::All},{"今日更新",View::Today},{"我的收藏",View::Starred}};
    for(std::size_t i=0;i<views.size();++i){
        const int x=40+static_cast<int>(i%2)*(w/2),y=130+static_cast<int>(i/2)*86;
        const bool selected=!feed_&&!group_&&search_.empty()&&view_==views[i].second;
        if(selected)c_.fill({x,y+12,4,48},25);
        c_.icon(i==0?Icon::Inbox:i==1?Icon::Book:i==2?Icon::Clock:Icon::Star,{x+24,y+21,30,30});
        c_.text(x+72,y+47,views[i].first,28);
        if(selected)c_.icon(Icon::Check,{x+w/2-124,y+24,24,24});
        hits_.push_back({{x,y,w/2-80,72},[this,v=views[i].second]{show_view(v);}});
    }
    c_.rule(56,325,w-112);
    button({40,335,140,65},"订阅",[this]{groups_=false;nav_page_=0;},!groups_,26,Icon::Rss);
    button({200,335,140,65},"分组",[this]{groups_=true;nav_page_=0;},groups_,26,Icon::Folder);
    button({w-250,335,200,65},"缓存管理",[this]{storage_=true;},false,26,Icon::Settings);
    const auto count=static_cast<std::size_t>(std::max(1,(h-720)/82));
    std::set<std::string> names;
    for(const auto& f:feeds_){
        names.insert(f.group);
        for(auto slash=f.group.find('/');slash!=std::string::npos;slash=f.group.find('/',slash+1))
            if(slash)names.insert(f.group.substr(0,slash));
    }
    const std::vector<std::string> groups(names.begin(),names.end());
    const auto total=groups_?groups.size():feeds_.size();
    for(std::size_t i=0;i<count;++i){
        const auto index=nav_page_*count+i;if(index>=total)break;
        if(groups_){
            const auto group=groups[index];
            const int y=405+static_cast<int>(i)*82;
            c_.icon(Icon::Folder,{64,y+22,28,28});
            c_.text(110,y+48,c_.ellipsize(group.empty()?"未分组":group,28,w-180),28);
            c_.rule(64,y+80,w-128,215);
            hits_.push_back({{40,y,w-80,74},[this,group]{show_group(group);}});
            continue;
        }
        const auto f=feeds_[index];
        auto label=(f.group.empty()?"":f.group+" / ")+f.title+(f.error.empty()?"":"  !");
        const int y=405+static_cast<int>(i)*82;
        c_.icon(Icon::Rss,{64,y+22,28,28});
        c_.text(110,y+48,c_.ellipsize(label,28,w-330),28);
        c_.rule(64,y+80,w-128,215);
        hits_.push_back({{40,y,w-260,74},[this,id=f.id]{show_view(View::All,id);}});
        button({w-200,y,150,74},"管理",[this,f]{managing_=f;},false,24,Icon::Settings);
    }
    button({40,h-280,170,65},"上一组",[this]{if(nav_page_)--nav_page_;});
    button({w-210,h-280,170,65},"下一组",[this,count,total]{if((nav_page_+1)*count<total)++nav_page_;});
    c_.rule(64,h-218,w-128,180);
    button({40,h-195,w/2-60,72},"导入订阅",[this]{request_="import";navigation_=false;},false,26,Icon::Download);
    button({w/2+20,h-195,w/2-60,72},"添加订阅",[this]{start_input("add","https://");navigation_=false;},false,26,Icon::Plus);
    button({40,h-105,w/2-60,72},"导出 OPML",[this]{request_="export";navigation_=false;},false,26,Icon::Upload);
    button({w/2+20,h-105,w/2-60,72},"退出应用",[this]{quit=true;},false,26,Icon::Exit);
}
void App::input_screen(){
    const int w=c_.width,h=c_.height;
    const auto title=input_mode_=="add"?"添加订阅":input_mode_=="title"?"订阅名称":input_mode_=="group"?"订阅分组":"搜索文章";
    const auto hint=input_mode_=="add"?"输入 RSS / Atom 地址":input_mode_=="title"?"输入显示名称":input_mode_=="group"?"留空表示未分组；使用 / 分隔多级分组":"搜索本地文章标题";
    c_.text(64,88,title,44);c_.text(64,150,hint,24,75);
    c_.rule(64,180,w-128,180);
    const auto frame=[this](Rect r){c_.fill(r,170);c_.fill({r.x+1,r.y+1,r.w-2,r.h-2},255);};
    frame({64,204,w-128,266});
    const auto lines=c_.wrap(input_,28,w-176);
    const auto first=lines.size()>5?lines.size()-5:0;
    for(std::size_t i=first;i<lines.size();++i)c_.text(88,246+static_cast<int>(i-first)*44,lines[i].text,28);
    const int cursor_x=88+c_.measure(lines.back().text,28);
    c_.fill({cursor_x,223+static_cast<int>(lines.size()-first-1)*44,2,28},30);
    c_.text(64,525,input_mode_=="add"?"支持 RSS / Atom，也可通过 OPML 批量导入。":"输入后点击确定；取消不会保存修改。",24,75);
    c_.rule(64,h-584,w-128,180);
    const std::vector<std::string> keys={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm","./:-_?=&%"};
    int y=h-560;
    for(const auto& row:keys){
        const int keyw=(w-64)/10;
        for(std::size_t i=0;i<row.size();++i){const std::string ch(1,shift_?static_cast<char>(std::toupper(static_cast<unsigned char>(row[i]))):row[i]);
            const Rect key{32+static_cast<int>(i)*keyw,y,keyw-4,70};
            frame(key);button(key,ch,[this,ch]{keyboard_key(ch);},false,28);}
        y+=76;
    }
    c_.rule(40,h-182,w-80,180);
    button({40,h-170,w/3-50,70},"取消",[this]{input_mode_.clear();input_.clear();},false,26,Icon::Close);
    button({w/3+10,h-170,w/3-30,70},"删除",[this]{backspace();});
    button({w*2/3+10,h-170,w/3-50,70},"确定",[this]{submit();},true,26,Icon::Check);
    button({40,h-86,w/3-50,70},chinese_?"清空拼音":shift_?"小写":"大写",[this]{if(chinese_){composing_.clear();candidate_page_=0;}else shift_=!shift_;});
    button({w/3+10,h-86,w/3-30,70},composing_.empty()?"空格":"选首词",[this]{input_space();});
    if(input_mode_!="add")button({w*2/3+10,h-86,w/3-50,70},chinese_?"切换英文":"切换中文",[this]{
        if(!composing_.empty()){const auto raw=composing_;text_input(raw);if(!composing_.empty())return;}
        chinese_=!chinese_;shift_=false;
    });
    if(chinese_){
        c_.text(64,588,composing_.empty()?"拼音输入 · ü 用 v":"拼音  ·  "+composing_,28);
        const auto candidates=pinyin_candidates(composing_);
        const auto pages=(candidates.size()+5)/6;
        if(candidate_page_>=pages)candidate_page_=0;
        for(std::size_t i=candidate_page_*6;i<std::min(candidates.size(),(candidate_page_+1)*6);++i){
            const auto slot=i%6;const auto word=candidates[i];
            const Rect candidate{40+static_cast<int>(slot%3)*330,620+static_cast<int>(slot/3)*80,316,70};
            c_.rule(candidate.x+16,candidate.y+candidate.h-1,candidate.w-32,180);
            button(candidate,word,[this,word]{text_input(word);},false,30);
        }
        if(!composing_.empty()&&candidates.empty())c_.text(56,665,"暂无候选，请继续输入或删除拼音。",26,100);
        if(pages>1){
            button({40,790,230,64},"上一组",[this]{if(candidate_page_)--candidate_page_;});
            c_.text(420,834,std::to_string(candidate_page_+1)+" / "+std::to_string(pages),26,100);
            button({790,790,230,64},"下一组",[this,pages]{if(candidate_page_+1<pages)++candidate_page_;});
        }
    }
}
void App::info_screen(){
    const int w=c_.width,h=c_.height;
    c_.text(56,88,"文章信息",40);
    button({w-190,30,140,76},"返回",[this]{info_=false;},false,26,Icon::Back);
    const auto details=current_->title+"\n\n来源："+source(current_->feed_id)+"\n作者："+
        (current_->author.empty()?"未提供":current_->author)+"\n\n原文地址：\n"+current_->url+
        "\n\n当前显示订阅源提供的内容。\n图片按原文位置显示，点击可查看大图；未自动提取网页全文。";
    const auto lines=c_.wrap(details,28,w-112);int y=180;
    for(const auto& line:lines){if(y>h-100)break;c_.text(56,y,line.text,28);y+=44;}
}
bool App::draw_image(const ArticleImage& image,Rect area){
    if(image.width<=0||image.height<=0||image.width>1072||image.height>1448||
       image.pixels.size()!=static_cast<std::size_t>(image.width)*image.height)return false;
    const double scale=std::min({1.0,double(area.w)/image.width,double(area.h)/image.height});
    const int width=std::max(1,static_cast<int>(image.width*scale)),height=std::max(1,static_cast<int>(image.height*scale));
    const int left=area.x+(area.w-width)/2,top=area.y+(area.h-height)/2;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x)
        c_.pixels[(top+y)*c_.width+left+x]=image.pixels[(y*image.height/height)*image.width+x*image.width/width];
    return true;
}
void App::image_screen(){
    const int w=c_.width,h=c_.height;
    c_.text(56,76,"文章图片",36);
    button({w-190,25,140,76},"返回",[this]{images_=false;},false,26,Icon::Back);
    const auto& reference=current_->images.at(image_page_);
    auto cached=store_.image(current_->id,reference.url);
    if(cached&&draw_image(*cached,{56,130,w-112,h-318})){
    }else{
        const auto message=cached&&!cached->error.empty()?"图片未缓存：\n"+cached->error:"图片尚未缓存，返回列表同步后可离线查看。";
        int y=480;for(const auto& line:c_.wrap(message,28,w-112)){if(y>h-250)break;c_.text(56,y,line.text,28);y+=44;}
    }
    const auto caption=c_.ellipsize(reference.alt.empty()?"订阅源图片":reference.alt,24,w-112);
    c_.text((w-c_.measure(caption,24))/2,h-125,caption,24,90);
    c_.text(w/2-50,h-42,std::to_string(image_page_+1)+" / "+std::to_string(current_->images.size()),23,90);
    button({36,h-84,180,68},"上一张",[this]{previous();},false,26,Icon::Back);
    button({w-216,h-84,180,68},"下一张",[this]{next();},false,26,Icon::Next);
}
void App::management_screen(){
    const int w=c_.width;
    c_.text(64,88,storage_?"缓存管理":"订阅管理",44);
    c_.rule(64,126,w-128,180);
    button({w-190,30,140,76},"返回",[this]{back();},false,26,Icon::Back);
    std::string details;
    if(storage_){
        details="清理 30 天前的已读文章\n\n收藏和正在阅读的文章会保留。\n未读文章不受影响。\n\n清理会删除这些文章的本地内容和状态。\n订阅源仍提供的文章可能在同步后重新出现。";
        button({56,760,w-112,82},"清理旧文章…",[this]{confirmation_="clean";},true);
    }else{
        c_.icon(Icon::Rss,{64,168,32,32});
        const auto title=c_.wrap(managing_->title,36,w-176);
        for(std::size_t i=0;i<std::min<std::size_t>(2,title.size());++i)
            c_.text(112,198+static_cast<int>(i)*46,c_.ellipsize(title[i].text+(i==1&&title.size()>2?"…":""),36,w-176),36);
        c_.text(64,298,"订阅地址",24,75);
        const auto url=c_.wrap(managing_->url,26,w-128);
        for(std::size_t i=0;i<std::min<std::size_t>(2,url.size());++i)
            c_.text(64,340+static_cast<int>(i)*38,c_.ellipsize(url[i].text+(i==1&&url.size()>2?"…":""),26,w-128),26);
        c_.rule(64,410,w-128,200);
        c_.text(64,460,"分组",24,75);
        c_.text(240,460,c_.ellipsize(managing_->group.empty()?"未分组":managing_->group,28,w-304),28);
        c_.text(64,522,"最近同步",24,75);
        c_.text(240,522,managing_->synced_at?date(managing_->synced_at):"尚未成功同步",28);
        if(!managing_->error.empty()){
            c_.text(64,590,"同步未完成",26);
            const auto error=c_.wrap(managing_->error,24,w-128);
            for(std::size_t i=0;i<std::min<std::size_t>(2,error.size());++i)
                c_.text(64,630+static_cast<int>(i)*36,c_.ellipsize(error[i].text+(i==1&&error.size()>2?"…":""),24,w-128),24,75);
        }
        c_.rule(64,720,w-128,180);
        button({56,760,w-112,82},"本订阅全部标为已读…",[this]{confirmation_="read";},false,26,Icon::Check);
        button({56,880,w-112,82},"取消订阅…",[this]{confirmation_="remove";},false,26,Icon::Close);
        button({56,1000,w/2-76,82},"修改名称",[this]{start_input("title",managing_->title);},false,26,Icon::Settings);
        button({w/2+20,1000,w/2-76,82},"调整分组",[this]{start_input("group",managing_->group);},false,26,Icon::Folder);
        c_.rule(64,980,w-128,200);
    }
    int y=180;
    for(const auto& line:c_.wrap(details,28,w-112)){if(y>710)break;c_.text(64,y,line.text,28);y+=44;}
}
void App::confirmation_screen(){
    const int w=c_.width;
    c_.text(64,88,"确认操作",44);c_.rule(64,126,w-128,180);
    const auto message=confirmation_=="remove"?"取消这个订阅？\n\n收藏文章会保留。其余文章及阅读进度将从本机删除。":
        confirmation_=="read"?"将这个订阅的全部文章标为已读？\n\n收藏和阅读位置会保留。":
        "清理 30 天前的已读文章？\n\n收藏、未读和正在阅读的文章会保留。此操作无法撤销。";
    int y=250;for(const auto& line:c_.wrap(message,32,w-112)){c_.text(56,y,line.text,32);y+=50;}
    button({56,760,w/2-76,84},"取消",[this]{confirmation_.clear();});
    button({w/2+20,760,w/2-76,84},"确认",[this]{
        std::string result;
        if(confirmation_=="remove"){
            store_.remove_feed(managing_->id);
            if(feed_==managing_->id){feed_=0;view_=View::Unread;}
            result="已取消订阅，收藏文章已保留";
        }else if(confirmation_=="read"){
            store_.mark_feed_read(managing_->id);result="本订阅已全部标为已读";
        }else{
            const auto count=store_.clean_read(static_cast<std::int64_t>(std::time(nullptr))-30LL*86400);
            result="已清理 "+std::to_string(count)+" 篇旧文章";
        }
        confirmation_.clear();managing_.reset();storage_=false;navigation_=false;
        reload();notify(result);
    },true);
}
void App::start_input(const std::string& mode,const std::string& value){
    input_mode_=mode;input_=value;composing_.clear();candidate_page_=0;chinese_=mode!="add";shift_=false;
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
void App::submit(){
    if(!error_.empty())return;
    if(!composing_.empty()){show_error("请先选择候选词，再点击确定。");return;}
    if(input_mode_=="title"||input_mode_=="group"){
        const auto first=input_.find_first_not_of(" \t\r\n"),last=input_.find_last_not_of(" \t\r\n");
        const auto value=first==std::string::npos?"":input_.substr(first,last-first+1);
        const auto title=input_mode_=="title"?value:managing_->title;
        const auto group=input_mode_=="group"?value:managing_->group;
        store_.edit_feed(managing_->id,title,group);
        managing_->title=title;managing_->group=group;input_mode_.clear();reload();
    }
    else if(input_mode_=="search"){search_=input_;input_mode_.clear();list_page_=0;reload();}
    else if(input_mode_=="add"){
        if(input_.rfind("https://",0)!=0&&input_.rfind("http://",0)!=0)throw std::runtime_error("请输入 HTTP / HTTPS 订阅地址");
        Feed f;f.url=input_;f.title=input_;store_.add_feed(f);input_mode_.clear();reload();request_="sync";
    }
    dirty=true;
}
void App::tap(int x,int y){
    for(auto it=hits_.rbegin();it!=hits_.rend();++it)if(it->rect.contains(x,y)){
        auto action=it->action;action();dirty=true;return;
    }
}
void App::next(){
    if(images_){if(image_page_+1<current_->images.size())++image_page_;}
    else if(current_){if(page_+1<pages_.size()){++page_;save_position();}}
    else{ensure_list_page(list_page_+1);if(list_page_+1<list_starts_.size())++list_page_;}
    dirty=true;
}
void App::previous(){
    if(images_){if(image_page_)--image_page_;}
    else if(current_){if(page_){--page_;save_position();}}
    else if(list_page_)--list_page_;dirty=true;
}
void App::back(){
    if(!error_.empty())error_.clear();
    else if(!confirmation_.empty())confirmation_.clear();
    else if(!input_mode_.empty()){input_mode_.clear();}
    else if(managing_||storage_){managing_.reset();storage_=false;}
    else if(navigation_)navigation_=false;
    else if(images_)images_=false;
    else if(info_)info_=false;
    else if(menu_)menu_=false;
    else if(current_){save_position();current_.reset();pages_.clear();}
    dirty=true;
}
void App::notify(const std::string& text){status_=text;dirty=true;}
void App::show_error(const std::string& text){error_=text;dirty=true;}
std::string App::take_request(){auto request=request_;request_.clear();return request;}
}
