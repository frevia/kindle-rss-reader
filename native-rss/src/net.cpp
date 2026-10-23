#include "net.hpp"
#include "feed.hpp"
#include "image.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace rss {
namespace {
struct Transfer { Response response; bool too_large = false; };
constexpr std::size_t max_feed = 8 * 1024 * 1024;
size_t body_cb(char* data, size_t size, size_t count, void* context) {
    auto& t = *static_cast<Transfer*>(context);
    if (size && count > max_feed / size) { t.too_large = true; return 0; }
    const auto n = size * count;
    if (n > max_feed - t.response.body.size()) { t.too_large = true; return 0; }
    try { t.response.body.append(data, n); } catch (...) { return 0; }
    return n;
}
size_t header_cb(char* data, size_t size, size_t count, void* context) {
    const auto n = size * count;
    auto& r = static_cast<Transfer*>(context)->response;
    try {
        std::string line(data, n);
        if (line.rfind("HTTP/", 0) == 0) { r.etag.clear(); r.modified.clear(); }
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
        }
    } catch (...) { return 0; }
    return n;
}
int progress_cb(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(context)->load() ? 1 : 0;
}
}
Response fetch(const std::string& url, const std::string& etag, const std::string& modified,
               const std::atomic<bool>& cancel) {
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
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 5L);
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_CONNECTTIMEOUT, 10L);
    set(CURLOPT_TIMEOUT, 35L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
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
        if (t.too_large) throw std::runtime_error("订阅内容超过 8 MB 上限");
        throw std::runtime_error(std::string("网络请求失败：") + curl_easy_strerror(result));
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &t.response.status);
    char* effective = nullptr;
    curl_easy_getinfo(curl.get(), CURLINFO_EFFECTIVE_URL, &effective);
    if (effective) t.response.effective_url = effective;
    if (t.response.status != 200 && t.response.status != 304)
        throw std::runtime_error("服务器返回 HTTP " + std::to_string(t.response.status));
    return std::move(t.response);
}
void sync_feeds(Store& store, const std::atomic<bool>& cancel,
                const std::function<void(const SyncProgress&)>& progress) {
    const auto feeds = store.feeds();
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
    int image_failures=0;
    for(const auto& item:store.pending_images()){
        if(cancel)break;
        p.message="正在缓存文章图片…";progress(p);
        try{
            auto response=fetch(item.second.url,"","",cancel);
            if(cancel)break;
            store.save_image(item.first,decode_image(item.second,response.body));
        }catch(const std::exception& e){
            if(cancel)break;
            auto failed=item.second;failed.error=e.what();store.save_image(item.first,failed);++image_failures;
        }
    }
    p.message = cancel ? "同步已取消，已保存的内容仍可阅读" :
        (p.failed ? "同步完成 · " + std::to_string(p.failed) + " 个订阅失败" : "更新完成 · 点击查看新文章");
    if(!cancel&&image_failures)p.message+=" · "+std::to_string(image_failures)+" 张图片未缓存";
    progress(p);
}
}
