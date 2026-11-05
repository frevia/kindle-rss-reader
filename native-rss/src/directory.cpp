#include "directory.hpp"

#include "net.hpp"

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>

namespace rss {
namespace {
constexpr const char* kMetadataKey = "directory_catalogs_v1";
constexpr std::size_t kMaxMetadata = 256 * 1024;
constexpr std::size_t kMaxDirectories = 100;
constexpr std::size_t kMaxFeeds = 5000;

const std::vector<Directory>& builtins() {
    static const std::vector<Directory> items{
        {"blogs", "中文独立博客", "https://raw.githubusercontent.com/timqian/chinese-independent-blogs/master/feed.opml", DirectoryKind::Opml, true},
        {"routes", "Awesome RSSHub Routes", "https://raw.githubusercontent.com/JackyST0/awesome-rsshub-routes/main/feeds.opml", DirectoryKind::Opml, true},
    };
    return items;
}

std::string xml_string(xmlChar* value) {
    if (!value) return {};
    std::string out(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return out;
}

std::string normalize_url(const std::string& input,bool preserve_fragment=false) {
    if (input.empty() || input.size() > 2048 || std::any_of(input.begin(), input.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
        throw std::runtime_error("源库地址无效");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, input.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("源库地址无效");
    auto part = [&](CURLUPart name, unsigned flags = 0) {
        char* raw = nullptr;
        const auto result = curl_url_get(url.get(), name, &raw, flags);
        std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
        return result == CURLUE_OK ? std::string(raw) : std::string();
    };
    auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto authority_start = input.find("://");
    const auto authority_end = authority_start == std::string::npos ? std::string::npos : input.find_first_of("/?#", authority_start + 3);
    const auto authority = authority_start == std::string::npos ? std::string() : input.substr(authority_start + 3, authority_end - authority_start - 3);
    if ((scheme != "http" && scheme != "https") || host.empty() || authority.find('@') != std::string::npos ||
        !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty())
        throw std::runtime_error("源库地址仅支持不含账号密码的 HTTP(S) URL");
    if ((!preserve_fragment && curl_url_set(url.get(), CURLUPART_FRAGMENT, nullptr, 0) != CURLUE_OK) ||
        curl_url_set(url.get(), CURLUPART_SCHEME, scheme.c_str(), 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_HOST, host.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("源库地址无效");
    const auto port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
    const auto default_port = (scheme == "http" && port == "80") || (scheme == "https" && port == "443");
    if (default_port && curl_url_set(url.get(), CURLUPART_PORT, nullptr, 0) != CURLUE_OK)
        throw std::runtime_error("源库地址无效");
    char* raw = nullptr;
    if (curl_url_get(url.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) throw std::runtime_error("源库地址无效");
    std::unique_ptr<char, decltype(&curl_free)> result(raw, curl_free);
    std::string normalized(raw);
    if (normalized.size() > 2048) throw std::runtime_error("源库地址超过 2048 字节");
    return normalized;
}

bool valid_single_line(const std::string& text, std::size_t limit) {
    if (text.empty() || text.size() > limit) return false;
    return std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

xmlDocPtr read_xml(const std::string& text, std::size_t max_size) {
    if (text.empty() || text.size() > max_size) throw std::runtime_error("源库 XML 无效或超出大小限制");
    auto* doc = xmlReadMemory(text.data(), static_cast<int>(text.size()), nullptr, nullptr,
                              XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    if (!doc || doc->intSubset || doc->extSubset) {
        if (doc) xmlFreeDoc(doc);
        throw std::runtime_error("源库 XML 无效或包含不安全的 DTD");
    }
    return doc;
}

std::string dump_xml(xmlDocPtr doc, std::size_t max_size = kMaxMetadata) {
    xmlChar* raw = nullptr;
    int size = 0;
    xmlDocDumpMemoryEnc(doc, &raw, &size, "UTF-8");
    if (!raw || size < 0) { if (raw) xmlFree(raw); throw std::runtime_error("无法序列化源库设置"); }
    std::string result(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(size));
    xmlFree(raw);
    if (result.size() > max_size) throw std::runtime_error("源库 XML 超过大小限制");
    return result;
}

std::vector<Directory> read_custom(const Store& store) {
    const auto text = store.setting(kMetadataKey);
    if (text.empty()) return {};
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(read_xml(text, kMaxMetadata), xmlFreeDoc);
    auto* root = xmlDocGetRootElement(doc.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST "directories")) throw std::runtime_error("源库设置格式无效");
    std::vector<Directory> result;
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (!xmlStrEqual(node->name, BAD_CAST "directory") || result.size() == kMaxDirectories)
            throw std::runtime_error("源库设置格式无效或数量超过 100 个");
        Directory d;
        d.id = xml_string(xmlGetProp(node, BAD_CAST "id"));
        d.title = xml_string(xmlGetProp(node, BAD_CAST "title"));
        const auto kind = xml_string(xmlGetProp(node, BAD_CAST "kind"));
        if (kind != "opml" && kind != "website") throw std::runtime_error("源库类型无效");
        d.kind = kind == "opml" ? DirectoryKind::Opml : DirectoryKind::Website;
        d.url = normalize_url(xml_string(xmlGetProp(node, BAD_CAST "url")),d.kind==DirectoryKind::Website);
        if (d.id.empty() || d.id.size() > 128 || !valid_single_line(d.title, 512)) throw std::runtime_error("源库设置字段无效");
        result.push_back(std::move(d));
    }
    return result;
}

std::string new_id() {
    static std::mt19937_64 random(std::random_device{}());
    static std::uint64_t serial = 0;
    return "custom-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(++serial) + "-" + std::to_string(random());
}

void write_custom(Store& store, const std::vector<Directory>& dirs) {
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
    auto* root = xmlNewNode(nullptr, BAD_CAST "directories");
    if (!doc || !root) throw std::bad_alloc();
    xmlDocSetRootElement(doc.get(), root);
    for (const auto& d : dirs) {
        auto* node = xmlNewChild(root, nullptr, BAD_CAST "directory", nullptr);
        const char* kind = d.kind == DirectoryKind::Opml ? "opml" : "website";
        if (!node || !xmlNewProp(node, BAD_CAST "id", BAD_CAST d.id.c_str()) ||
            !xmlNewProp(node, BAD_CAST "title", BAD_CAST d.title.c_str()) ||
            !xmlNewProp(node, BAD_CAST "url", BAD_CAST d.url.c_str()) ||
            !xmlNewProp(node, BAD_CAST "kind", BAD_CAST kind)) throw std::bad_alloc();
    }
    store.set_setting(kMetadataKey, dump_xml(doc.get()));
}

std::string cache_key(const std::string& url) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string key = "directory_cache:";
    for (unsigned char c : url) { key.push_back(hex[c >> 4]); key.push_back(hex[c & 15]); }
    return key;
}

std::string encode_cache(std::int64_t updated_at, const std::string& opml) {
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
    auto* root = xmlNewNode(nullptr, BAD_CAST "directory-cache");
    if (!doc || !root) throw std::bad_alloc();
    xmlDocSetRootElement(doc.get(), root);
    const auto timestamp = std::to_string(updated_at);
    if (!xmlNewProp(root, BAD_CAST "updated-at", BAD_CAST timestamp.c_str())) throw std::bad_alloc();
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> source(read_xml(opml, 8 * 1024 * 1024), xmlFreeDoc);
    auto* payload = xmlDocCopyNode(xmlDocGetRootElement(source.get()), doc.get(), 1);
    if (!payload || !xmlAddChild(root, payload)) throw std::bad_alloc();
    return dump_xml(doc.get(), 8 * 1024 * 1024 + 4096);
}

DirectoryCache decode_cache(const std::string& text) {
    if (text.empty()) return {};
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(read_xml(text, 8 * 1024 * 1024 + 4096), xmlFreeDoc);
    auto* root = xmlDocGetRootElement(doc.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST "directory-cache")) throw std::runtime_error("源库缓存无效");
    const auto timestamp = xml_string(xmlGetProp(root, BAD_CAST "updated-at"));
    std::size_t consumed = 0;
    DirectoryCache result;
    try { result.updated_at = std::stoll(timestamp, &consumed); }
    catch (...) { throw std::runtime_error("源库缓存时间无效"); }
    if (consumed != timestamp.size() || result.updated_at < 0) throw std::runtime_error("源库缓存时间无效");
    xmlNode* payload = nullptr;
    for (auto* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        if (payload) throw std::runtime_error("源库缓存 OPML 无效");
        payload = node;
    }
    if (!payload || !xmlStrEqual(payload->name, BAD_CAST "opml")) throw std::runtime_error("源库缓存 OPML 无效");
    xmlBufferPtr buffer = xmlBufferCreate();
    if (!buffer) throw std::bad_alloc();
    xmlNodeDump(buffer, doc.get(), payload, 0, 0);
    std::string opml(reinterpret_cast<const char*>(xmlBufferContent(buffer)), xmlBufferLength(buffer));
    xmlBufferFree(buffer);
    result.feeds = parse_opml(opml,nullptr,kMaxFeeds);
    return result;
}

std::vector<Feed> clean_feeds(std::string_view body,const std::atomic<bool>& cancel) {
    auto parsed = parse_opml(body,nullptr,kMaxFeeds,&cancel);
    std::vector<Feed> clean;
    std::set<std::string> seen;
    for (auto feed : parsed) {
        if(cancel)throw std::runtime_error("已取消源库刷新");
        try { feed.url = normalize_url(feed.url); }
        catch (...) { continue; }
        if (feed.title.empty()) feed.title = feed.url;
        if (!valid_single_line(feed.title, 512) || feed.group.size() > 2048 ||
            std::any_of(feed.group.begin(), feed.group.end(), [](unsigned char c) { return c < 32 || c == 127; })) continue;
        if (seen.insert(feed.url).second) clean.push_back(std::move(feed));
    }
    return clean;
}
}

bool same_feed_url(const std::string& left,const std::string& right) {
    if(left==right)return true;
    try{return normalize_url(left)==normalize_url(right);}
    catch(const std::runtime_error&){return false;}
}

std::vector<Directory> directory_catalogs(const Store& store) {
    auto result = builtins();
    auto custom = read_custom(store);
    for (auto& item : custom) result.push_back(std::move(item));
    return result;
}

std::string save_directory(Store& store, const Directory& input) {
    const auto first=input.title.find_first_not_of(" \t\r\n"),last=input.title.find_last_not_of(" \t\r\n");
    const auto title=first==std::string::npos?std::string{}:input.title.substr(first,last-first+1);
    if (!valid_single_line(title, 512)) throw std::runtime_error("源库名称须为不超过 512 字节的单行文字");
    if (input.kind != DirectoryKind::Opml) throw std::runtime_error("仅支持 OPML 源库");
    Directory value = input;
    value.title = title;
    value.builtin = false;
    value.url = normalize_url(input.url, true);
    // libcurl can drop an empty query or fragment during normalization.
    const auto suffix_end = input.url.find_first_of("?#");
    auto suffix = value.url;
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (suffix_end != std::string::npos || suffix.size() < 5 || suffix.compare(suffix.size() - 5, 5, ".opml") != 0)
        throw std::runtime_error("源库地址必须以 .opml 结尾");
    auto custom = read_custom(store);
    if (input.id.empty()) {
        if (custom.size() >= kMaxDirectories) throw std::runtime_error("自定义源库最多 100 个");
        do { value.id = new_id(); } while (std::any_of(custom.begin(), custom.end(), [&](const Directory& d) { return d.id == value.id; }));
    } else {
        if (std::any_of(builtins().begin(), builtins().end(), [&](const Directory& d) { return d.id == input.id; }))
            throw std::runtime_error("不能修改内置源库");
        auto it = std::find_if(custom.begin(), custom.end(), [&](const Directory& d) { return d.id == input.id; });
        if (it == custom.end()) throw std::runtime_error("自定义源库不存在");
        value.id = input.id;
        *it = value;
    }
    for (const auto& d : directory_catalogs(store)) {
        if (d.id != value.id && normalize_url(d.url,d.kind==DirectoryKind::Website) == value.url) throw std::runtime_error("该源库地址已存在");
    }
    if (input.id.empty()) custom.push_back(value);
    write_custom(store, custom);
    return value.id;
}

void remove_directory(Store& store, const std::string& id) {
    if (std::any_of(builtins().begin(), builtins().end(), [&](const Directory& d) { return d.id == id; }))
        throw std::runtime_error("不能删除内置源库");
    auto custom = read_custom(store);
    const auto old_size = custom.size();
    custom.erase(std::remove_if(custom.begin(), custom.end(), [&](const Directory& d) { return d.id == id; }), custom.end());
    if (custom.size() == old_size) throw std::runtime_error("自定义源库不存在");
    write_custom(store, custom);
}

DirectoryCache directory_cache(const Store& store, const std::string& id) {
    const auto dirs = directory_catalogs(store);
    const auto it = std::find_if(dirs.begin(), dirs.end(), [&](const Directory& d) { return d.id == id; });
    if (it == dirs.end()) throw std::runtime_error("源库不存在");
    if (it->kind != DirectoryKind::Opml) return {};
    return decode_cache(store.setting(cache_key(normalize_url(it->url))));
}

void refresh_directory(Store& store, const std::string& id, const std::atomic<bool>& cancel) {
    const auto dirs = directory_catalogs(store);
    const auto it = std::find_if(dirs.begin(), dirs.end(), [&](const Directory& d) { return d.id == id; });
    if (it == dirs.end()) throw std::runtime_error("源库不存在");
    if (it->kind != DirectoryKind::Opml) throw std::runtime_error("网站入口不支持缓存刷新");
    if (cancel) throw std::runtime_error("已取消源库刷新");
    const auto snapshot_url = normalize_url(it->url);
    const auto response = fetch(snapshot_url, "", "", cancel);
    if (cancel) throw std::runtime_error("已取消源库刷新");
    auto feeds = clean_feeds(response.body,cancel);
    if (cancel) throw std::runtime_error("已取消源库刷新");
    const auto current = directory_catalogs(store);
    const auto latest = std::find_if(current.begin(), current.end(), [&](const Directory& d) { return d.id == id; });
    if (latest == current.end() || latest->kind != DirectoryKind::Opml || normalize_url(latest->url) != snapshot_url)
        throw std::runtime_error("源库已删除或地址已更改");
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const auto payload = encode_cache(now, export_opml(feeds));
    if (cancel) throw std::runtime_error("已取消源库刷新");
    store.set_setting(cache_key(snapshot_url), payload);
}
}
