#include "app.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace rss {
namespace {
std::string lowercase(std::string value){
    for(auto& c:value)if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string host(const std::string& url){
    const auto protocol=url.find("://");const auto start=protocol==std::string::npos?0:protocol+3;
    return url.substr(start,url.find('/',start)-start);
}
void outline(Canvas& canvas,Rect rect){
    canvas.fill(rect,150);canvas.fill({rect.x+1,rect.y+1,rect.w-2,rect.h-2},255);
}
std::vector<Directory> ordered_directories(const std::vector<Directory>& directories){
    auto ordered=directories;
    std::stable_sort(ordered.begin(),ordered.end(),[](const Directory& a,const Directory& b){return a.kind==DirectoryKind::Opml&&b.kind==DirectoryKind::Website;});
    return ordered;
}
std::size_t directory_rows_per_page(int height){return static_cast<std::size_t>(std::max(1,(height-184-280-96)/168));}
}
void App::load_directories(){
    directories_=directory_catalogs(store_);directory_caches_.clear();
    if(!directory_scope_.empty()&&std::none_of(directories_.begin(),directories_.end(),[this](const Directory& d){return d.id==directory_scope_&&d.kind==DirectoryKind::Opml;}))directory_scope_.clear();
    std::size_t total=0;
    for(const auto& directory:directories_){
        if(directory.kind!=DirectoryKind::Opml||(!directory_scope_.empty()&&directory.id!=directory_scope_))continue;
        auto cache=directory_cache(store_,directory.id);total+=cache.feeds.size();
        if(total>10000){directory_caches_.clear();directory_message_="合并源库超过 10000 个源，请在源库管理中选择单个源库。";break;}
        directory_caches_.emplace(directory.id,std::move(cache));
    }
    filter_directory_feeds();
}
void App::filter_directory_feeds(){
    discovery_feeds_.clear();std::set<std::string> seen;
    const auto query=lowercase(discovery_query_);
    for(const auto& directory:directories_){
        if(!directory_scope_.empty()&&directory.id!=directory_scope_)continue;
        const auto found=directory_caches_.find(directory.id);if(found==directory_caches_.end())continue;
        for(const auto& feed:found->second.feeds){
            if(discovery_topic_&&feed.group!=*discovery_topic_)continue;
            if(!query.empty()&&lowercase(feed.title+" "+feed.url+" "+feed.group+" "+directory.title).find(query)==std::string::npos)continue;
            if(seen.insert(feed.url).second)discovery_feeds_.push_back(feed);
        }
    }
    discovery_page_=0;
}
void App::open_discovery(){
    screen_=Screen::Discovery;directory_message_.clear();load_directories();
}
void App::directory_started(){directory_running_=true;directory_message_="正在下载源库，可返回继续阅读。";dirty=true;}
void App::directory_finished(bool cancelled,const std::string& error){
    directory_running_=false;
    directory_message_=cancelled?"源库下载已取消，原有缓存保留。":error.empty()?"源库已更新，可离线查找。":"源库更新未完成："+error;
    try{load_directories();}catch(const std::exception& e){directory_message_="无法读取源库："+std::string(e.what());}
    dirty=true;
}
void App::request_directory_refresh(){
    if(directory_running_)return;
    if(sync_state_==SyncState::Running||update_state_==UpdateState::Checking||update_state_==UpdateState::Downloading)
        throw std::runtime_error("请先等待当前同步、翻译或更新任务完成。");
    directory_started();request_="refresh-directory:"+directory_scope_;
}
void App::discovery_screen(){
    const int w=c_.width,h=c_.height;page_header("发现订阅");
    button({w-252,92,220,88},directory_running_?"取消下载":"刷新",[this]{if(directory_running_){request_="cancel-directory";directory_message_="正在取消源库下载…";}else request_directory_refresh();},false,28,Icon::None,true,Align::Right);
    outline(c_,{32,212,w-64,104});
    button({48,220,w-268,88},discovery_query_.empty()?"搜索名称、主题或网址":discovery_query_,[this]{start_input("directory-search",discovery_query_);},false,32,Icon::Search,true,Align::Left);
    button({w-196,220,148,88},discovery_query_.empty()?"查找":"清除",[this]{if(discovery_query_.empty())start_input("directory-search","");else{discovery_query_.clear();filter_directory_feeds();}},false,28,Icon::None,true,Align::Right);
    std::string scope="全部可搜索源库";
    for(const auto& directory:directories_)if(directory.id==directory_scope_)scope=directory.title;
    button({32,324,w-64,64},"源库范围："+scope,[this]{directory_scope_page_=0;screen_=Screen::DirectoryScope;},false,24,Icon::Folder,true,Align::Left);
    button({32,396,w-64,88},"主题："+(discovery_topic_?discovery_topic_->empty()?"未分类":*discovery_topic_:"全部"),[this]{directory_topic_page_=0;screen_=Screen::DirectoryTopics;},false,28,Icon::Folder,true,Align::Left);
    c_.text(32,522,std::to_string(discovery_feeds_.size())+" 个订阅源 · 本地源库",24,85);
    const int top=550,row_height=144,rows=std::max(1,(h-160-top)/row_height);
    discovery_page_count_=std::max<std::size_t>(1,(discovery_feeds_.size()+rows-1)/rows);
    discovery_page_=std::min(discovery_page_,discovery_page_count_-1);
    c_.rule(32,top-1,w-64,150);
    for(std::size_t i=discovery_page_*rows;i<std::min(discovery_feeds_.size(),(discovery_page_+1)*rows);++i){
        const auto feed=discovery_feeds_[i];const int y=top+static_cast<int>(i-discovery_page_*rows)*row_height;
        auto existing=std::find_if(feeds_.begin(),feeds_.end(),[&](const Feed& f){return same_feed_url(f.url,feed.url);});
        const bool subscribed=existing!=feeds_.end();
        auto candidate=feed;candidate.group.clear();if(subscribed)candidate=*existing;
        button({32,y+8,w-264,72},feed.title.empty()?host(feed.url):feed.title,[this,candidate]{edit_feed(candidate);},false,32,Icon::None,true,Align::Left);
        const auto metadata=(feed.group.empty()?"未分类":feed.group)+" · "+host(feed.url);
        c_.text(32,y+120,c_.ellipsize(metadata,24,w-280),24,85);
        button({w-220,y+28,188,88},subscribed?"已订阅":"订阅",[this,candidate]{edit_feed(candidate);},false,28,Icon::None,!subscribed,Align::Right);
        c_.rule(32,y+row_height-1,w-64,205);
    }
    if(discovery_feeds_.empty()){
        const bool cached=std::any_of(directory_caches_.begin(),directory_caches_.end(),[](const auto& item){return item.second.updated_at>0;});
        c_.text(32,684,cached?"没有找到相关订阅源":"先下载一份订阅源库",36);
        int y=756;const auto copy=cached?"试试其他关键词或主题，也可以切换源库。":"连接 Wi-Fi 后点击右上角刷新。下载后可以离线查找，再选择喜欢的网站订阅。";
        for(const auto& line:c_.wrap(copy,28,w-64)){c_.text(32,y,line.text,28,85);y+=44;}
    }
    if(!directory_message_.empty()){
        int y=h-144;const auto lines=c_.wrap(directory_message_,24,w-64);
        for(std::size_t i=0;i<std::min<std::size_t>(2,lines.size());++i)c_.text(32,y+static_cast<int>(i)*32,c_.ellipsize(lines[i].text+(i==1&&lines.size()>2?"…":""),24,w-64),24,75);
    }
    navigation_pager(discovery_page_,discovery_page_count_);
}
void App::directories_screen(){
    const int w=c_.width,h=c_.height;page_header("订阅源库");
    button({w-252,92,220,88},"添加",[this]{editing_directory_=Directory{};screen_=Screen::DirectoryEdit;},false,28,Icon::Plus,!directory_running_,Align::Right);
    const auto custom=std::count_if(directories_.begin(),directories_.end(),[](const Directory& d){return !d.builtin;});
    c_.text(32,224,"内置 "+std::to_string(directories_.size()-custom)+" 个 · 自定义 "+std::to_string(custom)+" 个",24,85);
    const auto ordered=ordered_directories(directories_);
    const auto rows=directory_rows_per_page(h);
    discovery_page_count_=std::max<std::size_t>(1,(ordered.size()+rows-1)/rows);
    directory_page_=std::min(directory_page_,discovery_page_count_-1);
    const auto begin=directory_page_*rows,end=std::min(ordered.size(),begin+rows);
    std::size_t index=begin;int y=280;
    while(index<end){
        const auto kind=ordered[index].kind;
        c_.fill({32,y,w-64,48},235);
        c_.text(48,y+32,kind==DirectoryKind::Opml?"OPML 源库":"旧版网站入口",24,85);y+=48;
        while(index<end&&ordered[index].kind==kind){
            const auto directory=ordered[index];
            c_.text(32,y+48,c_.ellipsize(directory.title,32,w-280),32);
            c_.text(32,y+92,std::string(directory.builtin?"内置":"自定义")+" · "+(directory.kind==DirectoryKind::Opml?"OPML 订阅列表":"外部网站浏览入口"),24,85);
            c_.text(32,y+132,c_.ellipsize(host(directory.url),24,w-280),24,85);
            button({w-220,y+4,188,76},directory.kind==DirectoryKind::Opml?"查看":"浏览",[this,directory]{
                if(directory.kind==DirectoryKind::Opml){directory_detail_id_=directory.id;screen_=Screen::DirectoryDetail;}
                else{directory_website_id_=directory.id;screen_=Screen::DirectoryWebsite;}
            },false,28,Icon::None,true,Align::Right);
            if(!directory.builtin)button({w-220,y+86,188,72},"编辑",[this,directory]{editing_directory_=directory;screen_=Screen::DirectoryEdit;},false,26,Icon::None,!directory_running_,Align::Right);
            c_.rule(32,y+167,w-64,205);y+=168;++index;
        }
    }
    if(directory_running_)c_.text(32,h-152,"源库正在下载，可返回发现页取消。",24,85);
    navigation_pager(directory_page_,discovery_page_count_);
}
void App::directory_edit_screen(){
    if(!editing_directory_){screen_=Screen::Directories;directories_screen();return;}
    const int w=c_.width,h=c_.height;const bool existing=!editing_directory_->id.empty();
    page_header(existing?"编辑源库":"添加源库");
    button({w-252,92,220,88},"保存",[this,h]{
        if(directory_running_)throw std::runtime_error("请先取消源库下载，再修改源库。");
        auto draft=*editing_directory_;draft.kind=DirectoryKind::Opml;
        const auto id=save_directory(store_,draft);editing_directory_.reset();directory_message_="源库已保存，下载后即可查找。";
        load_directories();screen_=Screen::Directories;
        const auto ordered=ordered_directories(directories_);const auto it=std::find_if(ordered.begin(),ordered.end(),[&](const Directory& d){return d.id==id;});
        if(it!=ordered.end())directory_page_=static_cast<std::size_t>(it-ordered.begin())/directory_rows_per_page(h);
    },true,28,Icon::None,!directory_running_,Align::Right);
    c_.text(32,232,"添加 OPML 订阅源列表，下载后挑选订阅。",24,85);
    c_.text(32,306,"源库名称",24,85);outline(c_,{32,332,w-64,104});
    button({48,340,w-96,88},editing_directory_->title.empty()?"点击输入源库名称":editing_directory_->title,[this]{start_input("directory-name",editing_directory_->title);},false,32,Icon::None,true,Align::Left);
    c_.text(32,504,"OPML 文件地址",24,85);outline(c_,{32,530,w-64,156});
    auto lines=c_.wrap(editing_directory_->url.empty()?"https://":editing_directory_->url,28,w-112);
    for(std::size_t i=0;i<std::min<std::size_t>(3,lines.size());++i)c_.text(56,572+static_cast<int>(i)*38,c_.ellipsize(lines[i].text+(i==2&&lines.size()>3?"…":""),28,w-112),28);
    hits_.push_back({{32,530,w-64,156},[this]{start_input("directory-url",editing_directory_->url);}});
    int y=760;const auto hint=editing_directory_->kind==DirectoryKind::Opml?"仅接受以 .opml 结尾的 HTTP(S) 文件地址，不含查询参数或 # 片段。下载后仍会验证 OPML 内容；普通 RSS 请到「我的订阅」添加。":"此为旧版网站入口。保存时须改为以 .opml 结尾的 HTTP(S) 文件地址，才能作为 OPML 源库使用。";
    for(const auto& line:c_.wrap(hint,28,w-64)){c_.text(32,y,line.text,28,85);y+=44;}
    if(existing){
        button({32,h-292,360,88},"移除这个源库",[this]{if(!directory_running_)confirmation_="remove-directory";},false,28,Icon::Close,!directory_running_,Align::Left);
        c_.text(32,h-164,"移除源库不会取消已经添加的订阅。",24,85);
    }
}
void App::directory_topics_screen(){
    const int w=c_.width,h=c_.height;page_header("订阅源主题");
    c_.text(32,224,"按源库提供的分类查找",24,85);
    std::set<std::string> topics;
    for(const auto& item:directory_caches_)for(const auto& feed:item.second.feeds)topics.insert(feed.group);
    std::vector<std::optional<std::string>> options{std::nullopt};for(const auto& topic:topics)options.push_back(topic);
    const int top=274,row_height=120,rows=std::max(1,(h-104-top)/row_height);
    discovery_page_count_=std::max<std::size_t>(1,(options.size()+rows-1)/rows);directory_topic_page_=std::min(directory_topic_page_,discovery_page_count_-1);
    for(std::size_t i=directory_topic_page_*rows;i<std::min(options.size(),(directory_topic_page_+1)*rows);++i){
        const auto topic=options[i];const int y=top+static_cast<int>(i-directory_topic_page_*rows)*row_height;
        button({32,y,w-64,row_height},std::string(topic==discovery_topic_?"✓ ":"")+(topic?topic->empty()?"未分类":*topic:"全部主题"),[this,topic]{discovery_topic_=topic;filter_directory_feeds();screen_=Screen::Discovery;},false,32,Icon::None,true,Align::Left);
        c_.rule(32,y+row_height-1,w-64,205);
    }
    navigation_pager(directory_topic_page_,discovery_page_count_);
}
void App::directory_scope_screen(){
    const int w=c_.width,h=c_.height;page_header("源库范围");
    c_.text(32,224,"选择用于查找订阅源的 OPML 源库",24,85);
    std::vector<std::pair<std::string,std::string>> options{{"","全部 OPML 源库"}};
    for(const auto& directory:ordered_directories(directories_))if(directory.kind==DirectoryKind::Opml)options.emplace_back(directory.id,directory.title);
    const int top=280,row_height=144,rows=std::max(1,(h-184-top)/row_height);
    discovery_page_count_=std::max<std::size_t>(1,(options.size()+rows-1)/rows);directory_scope_page_=std::min(directory_scope_page_,discovery_page_count_-1);
    const auto begin=directory_scope_page_*rows,end=std::min(options.size(),(directory_scope_page_+1)*rows);
    for(std::size_t i=begin;i<end;++i){
        const auto option=options[i];const int y=top+static_cast<int>(i-begin)*row_height;
        const bool selected=option.first==directory_scope_;
        button({32,y,w-64,row_height},std::string(selected?"✓ ":"")+option.second,[this,id=option.first]{directory_scope_=id;discovery_topic_.reset();discovery_query_.clear();discovery_page_=0;directory_scope_page_=0;load_directories();screen_=Screen::Discovery;},false,30,Icon::None,true,Align::Left);
        c_.rule(32,y+row_height-1,w-64,205);
    }
    navigation_pager(directory_scope_page_,discovery_page_count_);
}
void App::directory_detail_screen(){
    const int w=c_.width,h=c_.height;const auto it=std::find_if(directories_.begin(),directories_.end(),[this](const Directory& d){return d.id==directory_detail_id_;});
    if(it==directories_.end()){screen_=Screen::Directories;directories_screen();return;}
    page_header("源库详情");
    if(!it->builtin)button({w-252,92,220,88},"编辑",[this,directory=*it]{editing_directory_=directory;screen_=Screen::DirectoryEdit;},false,28,Icon::None,!directory_running_,Align::Right);
    int y=240;
    c_.text(32,y,c_.ellipsize(it->title,36,w-64),36);y+=76;
    c_.text(32,y,it->builtin?"内置 OPML 源库":"自定义 OPML 源库",26,85);y+=64;
    c_.text(32,y,"OPML 订阅列表地址",24,85);y+=48;
    const int url_bottom=h-268;
    const auto lines=c_.wrap(it->url,26,w-64);
    const auto visible=static_cast<std::size_t>(std::max(0,(url_bottom-y-36)/40+1));
    for(std::size_t i=0;i<std::min(lines.size(),visible);++i){c_.text(32,y,c_.ellipsize(lines[i].text+(i+1==visible&&lines.size()>visible?"…":""),26,w-64),26,85);y+=40;}
    c_.text(32,h-236,it->builtin?"内置地址由应用维护。":"此源库可用于应用内查找。",24,85);
    c_.rule(32,h-144,w-64,205);
    button({32,h-136,w-64,112},"查看订阅源",[this,id=it->id]{directory_scope_=id;discovery_topic_.reset();discovery_query_.clear();discovery_page_=0;load_directories();screen_=Screen::Discovery;},true,30,Icon::Search,true,Align::Center);
}
void App::directory_website_screen(){
    const int w=c_.width,h=c_.height;page_header("网站入口");
    const auto it=std::find_if(directories_.begin(),directories_.end(),[this](const Directory& d){return d.id==directory_website_id_;});
    if(it==directories_.end()){screen_=Screen::Directories;directories_screen();return;}
    int y=260;for(const auto& line:c_.wrap(it->title,40,w-64)){c_.text(32,y,line.text,40);y+=56;}
    y+=40;for(const auto& line:c_.wrap("此网站作为浏览入口，不参与应用内搜索。在网站上找到 RSS 地址后，可回到阅读器手动添加。",28,w-64)){c_.text(32,y,line.text,28,85);y+=44;}
    y+=48;for(const auto& line:c_.wrap(it->url,28,w-64)){if(y>h-360)break;c_.text(32,y,line.text,28);y+=40;}
    c_.text(32,h-260,"将离开阅读器，交由系统浏览器打开。",24,85);
    c_.rule(32,h-144,w-64,205);
    button({32,h-136,w-64,112},"打开浏览器",[this,url=it->url]{
        if(directory_running_||sync_state_==SyncState::Running||update_state_==UpdateState::Checking||update_state_==UpdateState::Downloading)throw std::runtime_error("请先等待当前后台任务完成。");
        store_.set_setting("directory_browser_url",url);open_website=url;quit=true;
    },true,30);
}
}
