#include "fulltext.hpp"

#include "feed.hpp"
#include <libxml/HTMLparser.h>
#include <libxml/URI.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace rss {
namespace {
constexpr std::size_t kInputLimit = 8 * 1024 * 1024;
constexpr std::size_t kOutputLimit = 512 * 1024;
constexpr std::size_t kNodeLimit = 10000;
constexpr std::size_t kDepthLimit = 64;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string prop(xmlNode* n, const char* key) {
    xmlChar* p = xmlGetProp(n, BAD_CAST key);
    if (!p) return {};
    std::string s(reinterpret_cast<const char*>(p)); xmlFree(p); return s;
}

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { if(c==0)return false; ++i; continue; }
        std::size_t n = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xC2 && c <= 0xDF ? 2 : 0;
        if (!n || i + n > s.size()) return false;
        for (std::size_t j = 1; j < n; ++j) if ((static_cast<unsigned char>(s[i+j]) & 0xC0) != 0x80) return false;
        const auto b = static_cast<unsigned char>(s[i+1]);
        if ((c == 0xE0 && b < 0xA0) || (c == 0xED && b >= 0xA0) || (c == 0xF0 && b < 0x90) || (c == 0xF4 && b >= 0x90)) return false;
        i += n;
    }
    return true;
}

bool utf8_charset(std::string s) {
    s = lower(std::move(s));
    const auto marker = s.find("charset=");
    if (marker != std::string::npos) s.erase(0, marker + 8);
    const auto end = s.find_first_of("; ,\t\r\n");
    if (end != std::string::npos) s.resize(end);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) || c == '\'' || c == '"'; }), s.end());
    return s.empty() || s == "utf-8" || s == "utf8" || s == "us-ascii" || s == "ascii";
}

std::string meta_charset(xmlNode* root) {
    std::vector<xmlNode*> todo{root};
    while (!todo.empty()) {
        auto* n = todo.back(); todo.pop_back();
        if (!n) continue;
        if (n->type == XML_ELEMENT_NODE && xmlStrcasecmp(n->name, BAD_CAST "meta") == 0) {
            auto charset = prop(n, "charset");
            if (charset.empty() && lower(prop(n, "http-equiv")) == "content-type") {
                auto content = lower(prop(n, "content")); auto pos = content.find("charset=");
                if (pos != std::string::npos) charset = content.substr(pos + 8);
            }
            if (!charset.empty()) return charset;
        }
        for (auto* c = n->children; c; c = c->next) todo.push_back(c);
    }
    return {};
}

bool excluded(const std::string& tag) {
    static const std::unordered_set<std::string> names = {"script","style","template","iframe","object","embed","svg","math","video","audio","head","footer","nav","aside","form","noscript"};
    return names.count(tag) != 0;
}

bool noise(xmlNode* n) {
    if(xmlHasProp(n,BAD_CAST "hidden")||lower(prop(n,"aria-hidden"))=="true")return true;
    auto style=lower(prop(n,"style"));style.erase(std::remove_if(style.begin(),style.end(),[](unsigned char c){return std::isspace(c);}),style.end());
    if(style.find("display:none")!=std::string::npos||style.find("visibility:hidden")!=std::string::npos)return true;
    const auto hint = lower(prop(n, "class") + " " + prop(n, "id"));
    for (const char* word : {"navigation", "navbar", "menu", "sidebar", "related", "comment", "comments", "social", "share", "breadcrumb", "advert", "advertisement", "promo", "cookie"})
        for(auto pos=hint.find(word);pos!=std::string::npos;pos=hint.find(word,pos+1)){
            const auto end=pos+std::strlen(word);
            const auto boundary=[](unsigned char c){return !std::isalnum(c);};
            if((pos==0||boundary(hint[pos-1]))&&(end==hint.size()||boundary(hint[end])))return true;
        }
    return false;
}

bool safe_url(const std::string& raw, const std::string& base, std::string& out) {
    if (raw.empty() || raw.size() > 4096 || raw.find_first_of("\r\n") != std::string::npos || raw.find('\0') != std::string::npos) return false;
    xmlChar* resolved = xmlBuildURI(BAD_CAST raw.c_str(), base.empty() ? nullptr : BAD_CAST base.c_str());
    if (!resolved) return false;
    out.assign(reinterpret_cast<const char*>(resolved)); xmlFree(resolved);
    const auto l = lower(out);
    return (l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0) && out.size() <= 2048 && out.find_first_of("\r\n") == std::string::npos;
}

std::string escape(std::string_view s) {
    std::string out; out.reserve(std::min(s.size(),kOutputLimit));
    for (char c : s) {
        const char* entity=c=='&'?"&amp;":c=='<'?"&lt;":c=='>'?"&gt;":c=='"'?"&quot;":nullptr;
        const auto size=entity?std::strlen(entity):1;
        if(size>kOutputLimit-out.size())throw std::runtime_error("正文超过 512 KiB 上限");
        if(entity)out+=entity;else out+=c;
    }
    return out;
}

bool allowed(const std::string& tag) {
    static const std::unordered_set<std::string> names = {"article","main","div","section","p","br","hr","h1","h2","h3","h4","h5","h6","ul","ol","li","blockquote","pre","code","strong","b","em","i","u","s","del","ins","a","img","figure","figcaption","table","thead","tbody","tfoot","tr","th","td","caption","sup","sub","small","span"};
    return names.count(tag) != 0;
}

void append_bounded(std::string& out, const std::string& s) {
    if (s.size() > kOutputLimit - std::min(out.size(), kOutputLimit)) throw std::runtime_error("正文超过 512 KiB 上限");
    out += s;
}

std::string node_text(xmlNode* root) {
    std::string out;
    std::vector<xmlNode*> todo{root};
    while (!todo.empty()) {
        auto* n = todo.back(); todo.pop_back();
        if (!n) continue;
        if (n->type == XML_TEXT_NODE && n->content) {
            const std::string_view s(reinterpret_cast<const char*>(n->content));
            if (s.size() <= kOutputLimit - std::min(out.size(), kOutputLimit)) out.append(s); else break;
        }
        if (n->type == XML_ELEMENT_NODE && (excluded(lower(reinterpret_cast<const char*>(n->name))) || noise(n))) continue;
        for (auto* c = n->last; c; c = c->prev) todo.push_back(c);
    }
    return out;
}

struct Metrics { std::size_t chars = 0, links = 0, paragraphs = 0, headings = 0, breaks = 0; };
Metrics metrics(xmlNode* root) {
    Metrics m; std::vector<xmlNode*> todo{root};
    while (!todo.empty()) {
        auto* n = todo.back(); todo.pop_back(); if (!n) continue;
        if (n->type == XML_ELEMENT_NODE) {
            const std::string tag = lower(reinterpret_cast<const char*>(n->name)); if (excluded(tag)) continue;
            if (noise(n)) continue;
            if (tag == "p") ++m.paragraphs;
            if (tag == "br") ++m.breaks;
            if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') ++m.headings;
            if (tag == "a") {const auto text=node_text(n);m.links+=static_cast<std::size_t>(std::max(0,xmlUTF8Strlen(BAD_CAST text.c_str())));}
        }
        for (auto* c = n->last; c; c = c->prev) todo.push_back(c);
        if (n->type == XML_TEXT_NODE && n->content) m.chars += xmlUTF8Strlen(n->content) > 0 ? static_cast<std::size_t>(xmlUTF8Strlen(n->content)) : 0;
    }
    return m;
}

std::string image_src(xmlNode* n) {
    for (const char* key : {"data-src", "data-original", "src"}) { auto s = prop(n, key); if (!s.empty() && s.rfind("data:", 0) != 0) return s; }
    auto srcset = prop(n, "srcset");
    if (!srcset.empty()) { auto item = srcset.substr(0, srcset.find(',')); auto end = item.find_first_of(" \t\r\n"); return item.substr(0, end); }
    return {};
}

void serialize(xmlNode* root, const std::string& base, std::string& out) {
    static const std::unordered_set<std::string> voids = {"br","hr","img"};
    std::function<void(xmlNode*)> walk = [&](xmlNode* n) {
        for (; n; n = n->next) {
            if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
                if (n->content) append_bounded(out, escape(reinterpret_cast<const char*>(n->content)));
                continue;
            }
            if (n->type != XML_ELEMENT_NODE) continue;
            const std::string tag = lower(reinterpret_cast<const char*>(n->name));
            if (excluded(tag) || noise(n)) continue;
            const bool keep = allowed(tag);
            std::string open = keep ? "<" + tag : "";
            if (tag == "a" && keep) { std::string url; if (safe_url(prop(n, "href"), base, url)) open += " href=\"" + escape(url) + "\""; }
            if (tag == "img" && keep) {
                std::string url; if (!safe_url(image_src(n), base, url)) continue;
                open += " src=\"" + escape(url) + "\"";
                const auto alt = prop(n, "alt"); if (!alt.empty()) open += " alt=\"" + escape(alt) + "\"";
                const auto title = prop(n, "title"); if (!title.empty()) open += " title=\"" + escape(title) + "\"";
            }
            if (keep) { open += ">"; append_bounded(out, open); }
            walk(n->children);
            if (keep && !voids.count(tag)) append_bounded(out, "</" + tag + ">");
        }
    };
    walk(root);
}

std::string title_of(xmlNode* root) {
    std::vector<xmlNode*> todo{root};
    while (!todo.empty()) {
        auto* n = todo.back(); todo.pop_back(); if (!n) continue;
        if (n->type == XML_ELEMENT_NODE && (xmlStrcasecmp(n->name, BAD_CAST "title") == 0 ||
            (xmlStrcasecmp(n->name, BAD_CAST "meta") == 0 && lower(prop(n, "property")) == "og:title"))) {
            xmlChar* p = xmlNodeGetContent(n); std::string s = p ? reinterpret_cast<const char*>(p) : ""; if (p) xmlFree(p);
            if (!s.empty()) return s.substr(0, 1024);
        }
        for (auto* c = n->last; c; c = c->prev) todo.push_back(c);
    }
    return {};
}

std::string author_of(xmlNode* root) {
    std::vector<xmlNode*> todo{root};
    while (!todo.empty()) {
        auto* n = todo.back(); todo.pop_back(); if (!n) continue;
        if (n->type == XML_ELEMENT_NODE && xmlStrcasecmp(n->name, BAD_CAST "meta") == 0 && lower(prop(n, "name")) == "author") return prop(n, "content").substr(0, 1024);
        for (auto* c = n->last; c; c = c->prev) todo.push_back(c);
    }
    return {};
}

bool gate_page(const std::string& text, const std::string& title) {
    if(text.size()>1600)return false;
    auto body=lower(text),heading=lower(title);
    const auto first=body.find_first_not_of(" \t\r\n");if(first!=std::string::npos)body.erase(0,first);
    for (const char* phrase : {"captcha", "verify you are human", "checking your browser", "just a moment", "access denied", "sign in to continue", "log in to continue", "登录后继续", "请登录", "请输入验证码", "人机验证", "安全检查", "访问受限"})
        if (body.rfind(phrase,0)==0||heading==phrase) return true;
    return false;
}
}

Entry extract_fulltext(std::string_view html, const std::string& effective_url, const std::string& charset) {
    if (html.empty()) throw std::runtime_error("网页内容为空");
    if (html.size() > kInputLimit) throw std::runtime_error("网页超过 8 MiB 上限");
    if (!utf8_charset(charset)) throw std::runtime_error("暂不支持网页编码：" + charset);
    if (!valid_utf8(html)) throw std::runtime_error("网页不是有效的 UTF-8 文本");
    if (!charset.empty() && (lower(charset).find("ascii") != std::string::npos) &&
        std::any_of(html.begin(), html.end(), [](unsigned char c) { return c > 0x7f; }))
        throw std::runtime_error("网页声明为 ASCII，但包含非 ASCII 字符");
    if (std::count(html.begin(), html.end(), '<') > static_cast<std::ptrdiff_t>(kNodeLimit))
        throw std::runtime_error("网页标签数量超过 DOM 限制");
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(htmlReadMemory(html.data(), static_cast<int>(html.size()), effective_url.c_str(), "UTF-8",
        HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT), xmlFreeDoc);
    if (!doc || !xmlDocGetRootElement(doc.get())) throw std::runtime_error("无法解析网页 HTML");
    xmlNode* root = xmlDocGetRootElement(doc.get());
    struct Frame { xmlNode* node; std::size_t depth; bool hidden=false; };
    std::vector<Frame> todo{{root, 1}}; std::vector<xmlNode*> candidates;
    std::size_t count = 0;
    while (!todo.empty()) {
        auto f = todo.back(); todo.pop_back();
        if (!f.node) continue;
        if (++count > kNodeLimit) throw std::runtime_error("网页 DOM 节点超过 10000 上限");
        if (f.depth > kDepthLimit) throw std::runtime_error("网页 DOM 深度超过 64 层");
        if (f.node->type == XML_ELEMENT_NODE) {
            auto tag = lower(reinterpret_cast<const char*>(f.node->name));
            // libxml2's HTML4 recovery can place HTML5 article/main nodes under head.
            f.hidden=f.hidden||(excluded(tag)&&tag!="head")||noise(f.node);
            if (!f.hidden&&(tag == "article" || tag == "main" || tag == "div" || tag == "section" || tag == "body" || tag == "td")) candidates.push_back(f.node);
        }
        for (auto* c = f.node->last; c; c = c->prev) todo.push_back({c, f.depth + 1,f.hidden});
    }
    const auto declared = meta_charset(root);
    if (charset.empty() && !utf8_charset(declared)) throw std::runtime_error("暂不支持网页声明的编码：" + declared);
    if (charset.empty() && lower(declared).find("ascii") != std::string::npos &&
        std::any_of(html.begin(), html.end(), [](unsigned char c) { return c > 0x7f; }))
        throw std::runtime_error("网页声明为 ASCII，但包含非 ASCII 字符");
    std::string base = effective_url;
    // Resolve against the first valid HTML base element, when present.
    todo = {{root, 1}};
    while (!todo.empty()) {
        auto f = todo.back(); todo.pop_back(); auto* n = f.node; if (!n) continue;
        if (n->type == XML_ELEMENT_NODE && xmlStrcasecmp(n->name, BAD_CAST "base") == 0) {
            std::string b; if (safe_url(prop(n, "href"), effective_url, b)) { base = std::move(b); break; }
        }
        for (auto* c = n->last; c; c = c->prev) todo.push_back({c, f.depth + 1});
    }
    xmlNode* best = nullptr; double best_score = 0;
    for (auto* n : candidates) {
        auto m = metrics(n); if (m.chars < 3 || m.links*2>m.chars) continue;
        const auto tag = lower(reinterpret_cast<const char*>(n->name));
        // shortcut: legacy body/table layouts need multiple prose blocks; extend scoring when fixtures demand it.
        if((tag=="body"||tag=="td")&&(m.paragraphs+m.breaks/2<2||m.chars<80||m.links*5>m.chars))continue;
        double score = static_cast<double>(m.chars) - std::min<std::size_t>(m.links, m.chars) * 1.5 + m.paragraphs * 40 + m.headings * 18;
        if (tag == "article") score *= 1.35; else if (tag == "main") score *= 1.15;
        if (score >= best_score) { best = n; best_score = score; }
    }
    if (!best || best_score < 24) throw std::runtime_error("未找到可读的文章正文");
    Entry result; result.url = effective_url; result.title = title_of(root); result.author = author_of(root);
    serialize(best->children, base, result.html);
    if (result.html.size() > kOutputLimit) throw std::runtime_error("正文 HTML 超过 512 KiB 上限");
    result.text = html_to_text(result.html);
    if (result.text.size() > kOutputLimit) throw std::runtime_error("正文文本超过 512 KiB 上限");
    if (result.text.empty() || gate_page(result.text, result.title)) throw std::runtime_error("网页是登录、验证码或访问拦截页面");
    result.images = html_images(result.html, base);
    return result;
}
}
