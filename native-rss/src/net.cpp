#include "net.hpp"
#include "feed.hpp"
#include "fulltext.hpp"
#include "image.hpp"
#include "translation.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace rss {
namespace {
constexpr std::size_t max_feed = 8 * 1024 * 1024;
struct Transfer { Response response; bool too_large = false; std::size_t headers = 0,limit=max_feed; };
std::string origin(const std::string& url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if(!parsed || url.size()>2048 || std::any_of(url.begin(),url.end(),[](unsigned char c){return c<=32||c==127;}) ||
       curl_url_set(parsed.get(),CURLUPART_URL,url.c_str(),0)!=CURLUE_OK)
        throw std::runtime_error("无效的网页地址");
    const auto part=[&](CURLUPart field,unsigned flags=0){
        char* raw=nullptr;const auto code=curl_url_get(parsed.get(),field,&raw,flags);
        std::unique_ptr<char,decltype(&curl_free)> value(raw,curl_free);
        return code==CURLUE_OK?std::string(raw):std::string();
    };
    auto scheme=part(CURLUPART_SCHEME),host=part(CURLUPART_HOST);
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c){return std::tolower(c);});
    if((scheme!="http"&&scheme!="https")||host.empty()||!part(CURLUPART_USER).empty()||!part(CURLUPART_PASSWORD).empty())
        throw std::runtime_error("网页地址仅支持不含账号密码的 HTTP(S) URL");
    return scheme+"://"+host+":"+part(CURLUPART_PORT,CURLU_DEFAULT_PORT);
}
bool public_ipv4(std::uint32_t ip) {
    const auto first=ip>>24;
    return first!=0 && first!=10 && first!=127 && first<224 &&
        (ip&0xffc00000u)!=0x64400000u && (ip&0xffff0000u)!=0xa9fe0000u &&
        (ip&0xfff00000u)!=0xac100000u && (ip&0xffff0000u)!=0xc0a80000u &&
        (ip&0xffffff00u)!=0xc0000000u && (ip&0xfffe0000u)!=0xc6120000u;
}
struct AddressPolicy { bool allow_private=false,blocked=false; };
curl_socket_t open_socket(void* context,curlsocktype,curl_sockaddr* address) {
    auto& policy=*static_cast<AddressPolicy*>(context);
    bool allowed=policy.allow_private;
    if(!allowed&&address->family==AF_INET)
        allowed=public_ipv4(ntohl(reinterpret_cast<const sockaddr_in*>(&address->addr)->sin_addr.s_addr));
    if(!allowed&&address->family==AF_INET6){
        const auto& ip=reinterpret_cast<const sockaddr_in6*>(&address->addr)->sin6_addr;
        if(IN6_IS_ADDR_V4MAPPED(&ip)){
            std::uint32_t v4;std::memcpy(&v4,ip.s6_addr+12,4);allowed=public_ipv4(ntohl(v4));
        }else{
            allowed=(ip.s6_addr[0]&0xe0)==0x20;
            if(ip.s6_addr[0]==0x20&&(ip.s6_addr[1]==0x02||
               (ip.s6_addr[1]==0x01&&ip.s6_addr[2]==0&&ip.s6_addr[3]==0)))allowed=false;
        }
    }
    if(!allowed){policy.blocked=true;return CURL_SOCKET_BAD;}
    return ::socket(address->family,address->socktype,address->protocol);
}
size_t body_cb(char* data, size_t size, size_t count, void* context) {
    auto& t = *static_cast<Transfer*>(context);
    if (size && count > t.limit / size) { t.too_large = true; return 0; }
    const auto n = size * count;
    if (n > t.limit - t.response.body.size()) { t.too_large = true; return 0; }
    try { t.response.body.append(data, n); } catch (...) { return 0; }
    return n;
}
size_t header_cb(char* data, size_t size, size_t count, void* context) {
    auto& t=*static_cast<Transfer*>(context);
    if(size&&count>65536/size)return 0;
    const auto n = size * count;
    if(n>65536-t.headers)return 0;
    t.headers+=n;
    auto& r = t.response;
    try {
        std::string line(data, n);
        if (line.rfind("HTTP/", 0) == 0) { r.etag.clear(); r.modified.clear(); r.content_type.clear(); r.location.clear(); }
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c){ return std::tolower(c); });
            auto value = line.substr(colon + 1);
            const auto start = value.find_first_not_of(" \t");
            const auto end = value.find_last_not_of(" \r\n\t");
            value = start == std::string::npos ? "" : value.substr(start, end - start + 1);
            if (key == "etag") r.etag = value;
            if (key == "last-modified") r.modified = value;
            if (key == "content-type") r.content_type = value;
            if (key == "location") r.location = value;
        }
    } catch (...) { return 0; }
    return n;
}
int progress_cb(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(context)->load() ? 1 : 0;
}
Response fetch_once(const std::string& url, const std::string& etag, const std::string& modified,
                    const std::atomic<bool>& cancel, const std::string& source, long timeout_ms) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("网络初始化失败");
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        throw std::runtime_error("订阅地址仅支持 HTTP / HTTPS");
    if (url.find_first_of("\r\n") != std::string::npos || etag.find_first_of("\r\n") != std::string::npos ||
        modified.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("无效的网络参数");
    if (cancel) throw std::runtime_error("已取消同步");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("无法创建网络连接");
    Transfer t;
    AddressPolicy policy;
    if(!source.empty())policy.allow_private=origin(url)==origin(source);
    curl_slist* raw_headers = nullptr;
    auto add_header = [&](const std::string& value) {
        auto next = curl_slist_append(raw_headers, value.c_str());
        if (!next) { curl_slist_free_all(raw_headers); throw std::bad_alloc(); }
        raw_headers = next;
    };
    if (!etag.empty()) add_header("If-None-Match: " + etag);
    if (!modified.empty()) add_header("If-Modified-Since: " + modified);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(curl.get(), option, value) != CURLE_OK) throw std::runtime_error("网络选项配置失败");
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_USERAGENT, "KindleNativeRSS/0.1");
    set(CURLOPT_ACCEPT_ENCODING, "");
    set(CURLOPT_FOLLOWLOCATION, source.empty()?1L:0L);
    set(CURLOPT_MAXREDIRS, 5L);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_CONNECTTIMEOUT, 10L);
    set(CURLOPT_TIMEOUT_MS, timeout_ms);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if(!source.empty()){
        set(CURLOPT_PROXY, "");
        set(CURLOPT_OPENSOCKETFUNCTION, open_socket);
        set(CURLOPT_OPENSOCKETDATA, &policy);
    }
    const char* ca = std::getenv("RSS_CA_BUNDLE");
    if (ca && *ca) set(CURLOPT_CAINFO, ca);
    else if (std::ifstream("/mnt/us/native-rss/certs/ca-bundle.pem").good())
        set(CURLOPT_CAINFO, "/mnt/us/native-rss/certs/ca-bundle.pem");
    set(CURLOPT_WRITEFUNCTION, body_cb);
    set(CURLOPT_WRITEDATA, &t);
    set(CURLOPT_HEADERFUNCTION, header_cb);
    set(CURLOPT_HEADERDATA, &t);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, progress_cb);
    set(CURLOPT_XFERINFODATA, &cancel);
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        if (cancel) throw std::runtime_error("已取消同步");
        if (policy.blocked) throw std::runtime_error("已阻止网页访问未经配置的本机或内网地址");
        if (t.too_large) throw std::runtime_error("订阅内容超过 8 MB 上限");
        throw std::runtime_error(std::string("网络请求失败：") + curl_easy_strerror(result));
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &t.response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective);
    if (effective) t.response.effective_url = effective;
    if (t.response.status != 200 && t.response.status != 304 &&
        !(!source.empty()&&t.response.status>=300&&t.response.status<=399))
        throw std::runtime_error("服务器返回 HTTP " + std::to_string(t.response.status));
    return std::move(t.response);
}
Response fetch_resource(const std::string& url,const std::string& source,const std::atomic<bool>& cancel,long timeout_ms) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    std::string current=url;
    for(int redirects=0;redirects<=5;++redirects){
        origin(current);
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0)throw std::runtime_error("网页下载超时");
        auto response=fetch_once(current,"","",cancel,source,static_cast<long>(remaining));
        if(response.status==200)return response;
        if(response.status==304)throw std::runtime_error("服务器未返回网页正文");
        if(response.location.empty()||redirects==5)throw std::runtime_error("网页跳转次数过多或地址缺失");
        std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> resolved(curl_url(),curl_url_cleanup);
        if(!resolved||curl_url_set(resolved.get(),CURLUPART_URL,current.c_str(),0)!=CURLUE_OK||
           curl_url_set(resolved.get(),CURLUPART_URL,response.location.c_str(),0)!=CURLUE_OK)
            throw std::runtime_error("无效的网页跳转地址");
        char* raw=nullptr;
        if(curl_url_get(resolved.get(),CURLUPART_URL,&raw,0)!=CURLUE_OK)throw std::runtime_error("无效的网页跳转地址");
        std::unique_ptr<char,decltype(&curl_free)> address(raw,curl_free);current=raw;
    }
    throw std::runtime_error("网页跳转次数过多");
}
std::string feed_url(Store& store,std::int64_t feed_id){
    for(const auto& f:store.feeds())if(f.id==feed_id)return f.url;
    throw std::runtime_error("订阅已取消，未继续下载");
}
int download_fulltext(Store& store,const std::atomic<bool>& cancel,SyncProgress& p,
                      const std::function<void(const SyncProgress&)>& progress,bool failed_only,std::int64_t target=0){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    auto pending=store.pending_fulltext(failed_only,10);
    if(target){pending.clear();if(auto entry=store.entry(target,false))pending.push_back(*entry);}
    int done=0,failed=0;
    for(const auto& entry:pending){
        if(cancel)break;
        if(failed_only)store.queue_fulltext(entry.id);
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0)break;
        p.message="正在下载网页正文 · "+std::to_string(done+1)+" / "+std::to_string(pending.size());progress(p);
        try{
            auto response=fetch_resource(entry.url,feed_url(store,entry.feed_id),cancel,static_cast<long>(std::min<std::int64_t>(15000,remaining)));
            auto type=response.content_type;
            std::transform(type.begin(),type.end(),type.begin(),[](unsigned char c){return std::tolower(c);});
            const auto semicolon=type.find(';');auto mime=type.substr(0,semicolon);
            while(!mime.empty()&&std::isspace(static_cast<unsigned char>(mime.back())))mime.pop_back();
            if(mime!="text/html"&&mime!="application/xhtml+xml")throw std::runtime_error("服务器返回的内容不是 HTML 网页");
            std::string charset;
            if(const auto start=type.find("charset=");start!=std::string::npos){
                charset=type.substr(start+8);charset=charset.substr(0,charset.find(';'));
                const auto begin=charset.find_first_not_of(" \t\"'");const auto end=charset.find_last_not_of(" \t\"'");
                charset=begin==std::string::npos?"":charset.substr(begin,end-begin+1);
            }
            auto content=extract_fulltext(response.body,response.effective_url,charset);
            if(cancel)break;
            store.save_fulltext(entry.id,content);
        }catch(const std::exception& e){
            if(cancel)break;
            store.fail_fulltext(entry.id,e.what());++p.failed;++failed;
        }
        ++done;
    }
    p.message="网页正文已处理 "+std::to_string(done)+" 篇";
    if(failed)p.message+=" · "+std::to_string(failed)+" 篇失败";
    progress(p);return failed;
}
int cache_images(Store& store,const std::atomic<bool>& cancel,SyncProgress& p,
                 const std::function<void(const SyncProgress&)>& progress,std::int64_t target=0){
    auto pending=store.pending_images();
    if(target){
        pending.clear();
        if(auto entry=store.fulltext_entry(target))for(const auto& image:entry->images)if(image.width==0)pending.emplace_back(target,image);
    }
    int failures=0;
    for(const auto& item:pending){
        if(cancel)break;
        p.message="正在缓存文章图片…";progress(p);
        try{
            auto entry=store.entry(item.first,false);if(!entry)continue;
            auto response=fetch_resource(item.second.url,feed_url(store,entry->feed_id),cancel,15000);
            if(cancel)break;
            store.save_image(item.first,decode_image(item.second,response.body));
        }catch(const std::exception& e){
            if(cancel)break;
            auto failed=item.second;failed.error=e.what();store.save_image(item.first,failed);++failures;
        }
    }
    return failures;
}
}
Response fetch(const std::string& url,const std::string& etag,const std::string& modified,const std::atomic<bool>& cancel){
    return fetch_once(url,etag,modified,cancel,"",35000);
}
namespace {
std::string post_qwen(const QwenConfig& config,const std::string& input,const std::atomic<bool>& cancel,const std::vector<std::string>& protected_tokens) {
    validate_qwen_config(config);
    static const auto initialized=curl_global_init(CURL_GLOBAL_DEFAULT);
    if(initialized!=CURLE_OK)throw std::runtime_error("翻译网络初始化失败");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    if(!curl)throw std::runtime_error("无法创建翻译连接");
    const auto body=qwen_request(input,protected_tokens);
    curl_slist* raw=nullptr;
    for(const auto& header:{std::string("Content-Type: application/json"),"Authorization: Bearer "+config.api_key,std::string("Expect:")}) {
        auto next=curl_slist_append(raw,header.c_str());
        if(!next){curl_slist_free_all(raw);throw std::bad_alloc();}raw=next;
    }
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(raw,curl_slist_free_all);
    Transfer transfer;transfer.limit=256*1024;
    const auto set=[&](CURLoption option,auto value){if(curl_easy_setopt(curl.get(),option,value)!=CURLE_OK)throw std::runtime_error("翻译网络配置失败");};
    set(CURLOPT_URL,config.endpoint.c_str());set(CURLOPT_POST,1L);
    set(CURLOPT_HTTPHEADER,headers.get());set(CURLOPT_POSTFIELDS,body.c_str());set(CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
    // Authenticated requests never follow redirects or inherit a proxy from article download settings.
    set(CURLOPT_FOLLOWLOCATION,0L);set(CURLOPT_PROXY,"");set(CURLOPT_PROTOCOLS_STR,"http,https");
    set(CURLOPT_CONNECTTIMEOUT,10L);set(CURLOPT_TIMEOUT,45L);set(CURLOPT_NOSIGNAL,1L);
    set(CURLOPT_SSL_VERIFYPEER,1L);set(CURLOPT_SSL_VERIFYHOST,2L);set(CURLOPT_ACCEPT_ENCODING,"");
    if(const auto ca=std::getenv("RSS_CA_BUNDLE");ca&&*ca)set(CURLOPT_CAINFO,ca);
    else if(std::ifstream("/mnt/us/native-rss/certs/ca-bundle.pem").good())set(CURLOPT_CAINFO,"/mnt/us/native-rss/certs/ca-bundle.pem");
    set(CURLOPT_WRITEFUNCTION,body_cb);set(CURLOPT_WRITEDATA,&transfer);
    set(CURLOPT_HEADERFUNCTION,header_cb);set(CURLOPT_HEADERDATA,&transfer);
    set(CURLOPT_NOPROGRESS,0L);set(CURLOPT_XFERINFOFUNCTION,progress_cb);set(CURLOPT_XFERINFODATA,&cancel);
    if(cancel)throw std::runtime_error("翻译已取消");
    const auto result=curl_easy_perform(curl.get());
    if(cancel)throw std::runtime_error("翻译已取消");
    if(transfer.too_large)throw std::runtime_error("翻译响应超过 256 KiB 上限");
    if(result!=CURLE_OK)throw std::runtime_error("翻译连接失败，请检查网络后手动重试");
    long status=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    if(status==401||status==403)throw std::runtime_error("翻译服务鉴权失败，请检查 API Key、地域和模型权限");
    if(status==429)throw std::runtime_error("翻译请求达到服务限额，请稍后手动重试");
    if(status>=300&&status<400)throw std::runtime_error("翻译服务返回重定向，已停止发送密钥，请检查服务地址");
    if(status!=200)throw std::runtime_error("翻译服务返回 HTTP "+std::to_string(status)+"，请检查余额或稍后重试");
    return qwen_translation(transfer.response.body);
}
}
void translate_article(Store& store,std::int64_t entry_id,bool web,const QwenConfig& config,
                       const std::atomic<bool>& cancel,const std::function<void(const SyncProgress&)>& progress) {
    validate_qwen_config(config);
    const auto source=web?store.fulltext_entry(entry_id):store.entry(entry_id);
    if(!source)throw std::runtime_error("所选正文尚未下载或已删除");
    const auto document=translation_document(*source);
    store.begin_translation(entry_id,web,document.revision,document.blocks.size());
    auto translated=store.translation_blocks(entry_id,web,document.revision);
    SyncProgress p;p.total=static_cast<int>(document.blocks.size());
    p.completed=static_cast<int>(store.translation_status(entry_id,web,document.revision).completed);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(5);
    auto next_request=std::chrono::steady_clock::now();
    for(std::size_t i=0;i<document.blocks.size();++i) {
        if(cancel||std::chrono::steady_clock::now()>=deadline)break;
        if(!translated[i].empty())continue;
        p.message="正在翻译 · "+std::to_string(p.completed)+" / "+std::to_string(p.total)+" 段";progress(p);
        try {
            const auto& block=document.blocks[i];
            std::string html=block.html;
            if(!block.request.empty()) {
                std::vector<std::string> protected_tokens;
                for(const auto& item:block.protected_markup)protected_tokens.push_back(item.first);
                html=translate_block(block,[&](const std::string& input) {
                    while(!cancel&&std::chrono::steady_clock::now()<next_request&&std::chrono::steady_clock::now()<deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if(cancel||std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("本轮翻译已暂停");
                    next_request=std::chrono::steady_clock::now()+std::chrono::milliseconds(1100);
                    return post_qwen(config,input,cancel,protected_tokens);
                });
                // Cache the response with placeholders; restore protected source only when rendering.
                validate_translation(block,html);
            }
            if(cancel)break;
            store.save_translation_block(entry_id,web,document.revision,i,html);
            p.completed=static_cast<int>(store.translation_status(entry_id,web,document.revision).completed);
        }catch(const std::exception& e) {
            if(cancel||std::chrono::steady_clock::now()>=deadline)break;
            store.set_translation_state(entry_id,web,document.revision,"failed",e.what());
            p.failed=1;p.message=std::string("翻译未完成：")+e.what()+"；已完成段落已保存";progress(p);return;
        }
    }
    const bool complete=p.completed==p.total&&!cancel;
    store.set_translation_state(entry_id,web,document.revision,complete?"ready":"paused");
    p.message=complete?"翻译已保存，可在翻译面板切换模式":cancel?"翻译已取消，已完成段落已保存":"本轮翻译已暂停，点击继续翻译剩余段落";
    progress(p);
}
std::string translate_selection(Store& store,const SelectionJob& job,const QwenConfig& config,const std::atomic<bool>& cancel) {
    const auto display_entry=[&]() {
        const auto source=job.web?store.fulltext_entry(job.entry_id):store.entry(job.entry_id);
        if(!source)throw std::runtime_error("选中文章已删除或正文不可用，请重新选择");
        if(job.mode==ReadingMode::Original)return *source;
        const auto document=translation_document(*source);
        const auto blocks=store.translation_blocks(job.entry_id,job.web,document.revision);
        return translated_entry(*source,document,blocks,job.mode);
    };
    if(cancel)throw std::runtime_error("翻译已取消");
    if(translation_revision(display_entry())!=job.revision)throw std::runtime_error("正文已更新，请重新打开文章后选择文字");
    if(const auto cached=store.selection(job);cached&&!cached->translation.empty())return cached->translation;
    const auto translated=post_qwen(config,job.range.text,cancel,{});
    TranslationBlock plain;
    (void)validate_translation(plain,translated);
    if(cancel)throw std::runtime_error("翻译已取消");
    if(translation_revision(display_entry())!=job.revision)throw std::runtime_error("正文已更新，请重新打开文章后选择文字");
    store.save_selection_translation(job,translated);
    return translated;
}
void sync_fulltext(Store& store,std::int64_t entry_id,const std::atomic<bool>& cancel,
                   const std::function<void(const SyncProgress&)>& progress){
    if(store.fulltext_status(entry_id).state!="pending")throw std::runtime_error("文章未加入下载队列，或网页正文已保存");
    SyncProgress p;p.total=1;
    download_fulltext(store,cancel,p,progress,false,entry_id);
    const auto image_failures=cancel?0:cache_images(store,cancel,p,progress,entry_id);
    p.completed=cancel?0:1;
    p.message=cancel?"下载已取消，RSS 原文仍可阅读":p.failed?"网页正文下载失败，RSS 原文仍可阅读":"网页正文已保存，可切换阅读";
    if(!cancel&&image_failures)p.message+=" · "+std::to_string(image_failures)+" 张图片未缓存";
    progress(p);
}
void sync_feeds(Store& store, const std::atomic<bool>& cancel,
                const std::function<void(const SyncProgress&)>& progress, bool failed_only) {
    auto feeds = store.feeds();
    if(failed_only)feeds.erase(std::remove_if(feeds.begin(),feeds.end(),[](const Feed& f){return f.error.empty();}),feeds.end());
    SyncProgress p; p.total = static_cast<int>(feeds.size());
    progress(p);
    for (const auto& f : feeds) {
        if (cancel) break;
        p.message = "正在更新 · " + f.title;
        progress(p);
        try {
            auto r = fetch(f.url, f.etag, f.modified, cancel);
            if (cancel) break;
            if (r.status == 200) store.save_entries(f.id, parse_feed(r.body, r.effective_url));
            store.sync_result(f.id, "", r.status == 304 && r.etag.empty() ? f.etag : r.etag,
                r.status == 304 && r.modified.empty() ? f.modified : r.modified, std::time(nullptr));
        } catch (const std::exception& e) {
            if (cancel) break;
            ++p.failed;
            store.sync_result(f.id, e.what(), "", "", std::time(nullptr));
        }
        ++p.completed;
        progress(p);
    }
    const auto feed_failures=p.failed;
    const auto fulltext_failures=cancel?0:download_fulltext(store,cancel,p,progress,failed_only);
    const auto image_failures=cancel?0:cache_images(store,cancel,p,progress);
    p.message = cancel ? "同步已取消，已保存的内容仍可阅读" :
        (feed_failures ? "同步完成 · " + std::to_string(feed_failures) + " 个订阅失败" : "RSS 更新完成");
    if(!cancel&&fulltext_failures)p.message+=" · "+std::to_string(fulltext_failures)+" 篇网页正文失败";
    if(!cancel&&!store.pending_fulltext().empty())p.message+=" · 仍有网页正文待下载，再次同步继续";
    if(!cancel&&image_failures)p.message+=" · "+std::to_string(image_failures)+" 张图片未缓存";
    else if(!cancel&&!store.pending_images().empty())p.message+=" · 仍有图片待缓存，再次同步继续";
    progress(p);
}
}
