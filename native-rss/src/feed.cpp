#include "feed.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/URI.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rss {
namespace {
constexpr std::size_t kMaxXml = 8 * 1024 * 1024;

std::string value(xmlChar* p) {
    if (!p) return {};
    std::string s(reinterpret_cast<const char*>(p));
    xmlFree(p);
    return s;
}

std::string content(xmlNode* n) { return value(xmlNodeGetContent(n)); }

bool named(xmlNode* n, const char* name) {
    return n && n->type == XML_ELEMENT_NODE && xmlStrEqual(n->name, BAD_CAST name);
}

xmlNode* child(xmlNode* n, const char* name) {
    for (xmlNode* c = n ? n->children : nullptr; c; c = c->next)
        if (named(c, name)) return c;
    return nullptr;
}

std::string inner(xmlNode* n) {
    std::string out;
    for (xmlNode* c = n ? n->children : nullptr; c; c = c->next) {
        xmlBufferPtr b = xmlBufferCreate();
        if (!b) continue;
        xmlNodeDump(b, n->doc, c, 0, 0);
        out.append(reinterpret_cast<const char*>(xmlBufferContent(b)), xmlBufferLength(b));
        xmlBufferFree(b);
    }
    return out;
}

std::string prop(xmlNode* n, const char* key) { return value(xmlGetProp(n, BAD_CAST key)); }

std::string namespace_href(xmlNode* n) {
    return n && n->ns && n->ns->href ? reinterpret_cast<const char*>(n->ns->href) : "";
}

std::string child_text(xmlNode* n, const char* name) { return content(child(n, name)); }

std::string escape(const std::string& s);
std::string atom_content(xmlNode* n) {
    if (!n) return {};
    const auto type = prop(n, "type");
    if (type == "xhtml") return inner(n);
    if (type == "html" || type == "text/html") return content(n);
    return escape(content(n));
}

std::string resolve_url(xmlNode* n, const std::string& raw, const std::string& source) {
    if (raw.empty()) return {};
    xmlChar* b = xmlNodeGetBase(n->doc, n);
    std::string base = b ? reinterpret_cast<const char*>(b) : source;
    if (b) xmlFree(b);
    if (base.empty()) base = source;
    xmlChar* resolved = xmlBuildURI(BAD_CAST raw.c_str(), base.empty() ? nullptr : BAD_CAST base.c_str());
    return resolved ? value(resolved) : raw;
}

std::string atom_link(xmlNode* n) {
    for (xmlNode* c = n->children; c; c = c->next) {
        if (!named(c, "link")) continue;
        std::string rel = prop(c, "rel");
        if (rel.empty() || rel == "alternate") {
            std::string href = prop(c, "href");
            if (!href.empty()) return href;
        }
    }
    return {};
}

std::int64_t make_time(std::tm tm, int offset_seconds) {
    return static_cast<std::int64_t>(timegm(&tm)) - offset_seconds;
}

std::int64_t parse_date(const std::string& s) {
    if (s.empty()) return 0;
    std::tm tm{};
    const char* p = s.c_str();
    const char* formats[] = {"%a, %d %b %Y %H:%M:%S", "%a, %d %b %Y %H:%M",
                             "%d %b %Y %H:%M:%S", "%d %b %Y %H:%M"};
    for (const char* f : formats) {
        tm = {};
        char* end = strptime(p, f, &tm);
        if (!end) continue;
        while (*end == ' ') ++end;
        int offset = 0;
        if (*end == '+' || *end == '-') {
            int sign = *end++ == '+' ? 1 : -1;
            int hh = 0, mm = 0;
            if (std::strlen(end) >= 2 && std::isdigit(static_cast<unsigned char>(end[0])) && std::isdigit(static_cast<unsigned char>(end[1]))) {
                hh = (end[0] - '0') * 10 + end[1] - '0'; end += 2;
                if (*end == ':') ++end;
                if (std::strlen(end) >= 2 && std::isdigit(static_cast<unsigned char>(end[0])) && std::isdigit(static_cast<unsigned char>(end[1])))
                    mm = (end[0] - '0') * 10 + end[1] - '0';
                else continue;
                if (hh > 23 || mm > 59) continue;
                offset = sign * (hh * 3600 + mm * 60);
            } else continue;
        } else if (!std::strncmp(end, "GMT", 3) || !std::strncmp(end, "UTC", 3)) offset = 0;
        else if (!std::strncmp(end, "EST", 3)) offset = -5 * 3600;
        else if (!std::strncmp(end, "EDT", 3)) offset = -4 * 3600;
        else if (!std::strncmp(end, "CST", 3)) offset = -6 * 3600;
        else if (!std::strncmp(end, "CDT", 3)) offset = -5 * 3600;
        else if (!std::strncmp(end, "MST", 3)) offset = -7 * 3600;
        else if (!std::strncmp(end, "MDT", 3)) offset = -6 * 3600;
        else if (!std::strncmp(end, "PST", 3)) offset = -8 * 3600;
        else if (!std::strncmp(end, "PDT", 3)) offset = -7 * 3600;
        else if (*end && std::strcmp(end, "Z") && std::strcmp(end, "+00:00")) continue;
        return make_time(tm, offset);
    }
    // ISO 8601: timezone is applied arithmetically, so process-local TZ is irrelevant.
    tm = {};
    int y, mon, day, hour = 0, min = 0, sec = 0;
    const int fields = std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mon, &day, &hour, &min, &sec);
    if (fields < 3 || (s.size() > 10 && fields < 5) || mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60) return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
    auto tpos = s.find_first_of("Tt ");
    std::size_t z = tpos == std::string::npos ? s.size() : s.find_first_of("Zz+-", tpos + 1);
    int offset = 0;
    if (z != std::string::npos && (s[z] == '+' || s[z] == '-')) {
        int oh = 0, om = 0;
        const auto zone = s.substr(z + 1);
        const char* format = zone.find(':') == std::string::npos ? "%2d%2d" : "%2d:%2d";
        if (std::sscanf(zone.c_str(), format, &oh, &om) != 2 || oh > 23 || om > 59) return 0;
        offset = (s[z] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return make_time(tm, offset);
}

std::string fallback_title(const std::string& url) {
    xmlURIPtr u = xmlParseURI(url.c_str());
    if (u && u->server) { std::string s = u->server; xmlFreeURI(u); return s; }
    if (u) xmlFreeURI(u);
    auto slash = url.find_last_of('/');
    return slash == std::string::npos ? "Untitled feed" : url.substr(slash + 1);
}

std::string identity(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    std::ostringstream out; out << "content:" << std::hex << h;
    return out.str();
}

void entries(xmlNode* parent, const std::string& source, ParsedFeed& out, bool atom) {
    for (xmlNode* n = parent->children; n; n = n->next) {
        if (!named(n, atom ? "entry" : "item")) continue;
        Entry e;
        if (atom) {
            e.title = child_text(n, "title");
            e.guid = child_text(n, "id");
            e.url = resolve_url(n, atom_link(n), source);
            e.author = child_text(child(n, "author"), "name");
            xmlNode* c = child(n, "content");
            e.html = atom_content(c);
            if (e.html.empty()) e.html = atom_content(child(n, "summary"));
            e.published = parse_date(child_text(n, "published"));
            if (!e.published) e.published = parse_date(child_text(n, "updated"));
        } else {
            e.title = child_text(n, "title");
            e.guid = child_text(n, "guid");
            std::string link = child_text(n, "link");
            e.url = resolve_url(n, link, source);
            e.author = child_text(n, "author");
            for (xmlNode* c = n->children; c; c = c->next)
                if (named(c, "encoded") && namespace_href(c) == "http://purl.org/rss/1.0/modules/content/") { e.html = content(c); break; }
            if (e.html.empty()) { auto d = child(n, "description"); if (d) e.html = content(d); }
            e.published = parse_date(child_text(n, "pubDate"));
        }
        if (e.title.empty() && e.guid.empty() && e.url.empty() && e.html.empty() && e.author.empty() && !e.published) continue;
        e.text = html_to_text(e.html);
        e.images = html_images(e.html,e.url.empty()?source:e.url);
        if (e.title.empty()) {
            std::size_t end = std::min<std::size_t>(100, e.text.size());
            while (end < e.text.size() && end > 0 && (static_cast<unsigned char>(e.text[end]) & 0xc0) == 0x80) --end;
            e.title = e.url.empty() ? (e.text.empty() ? "无标题文章" : e.text.substr(0, end)) : e.url;
        }
        if (e.guid.empty()) e.guid = e.url.empty() ? identity(e.title + "\n" + e.html + "\n" + std::to_string(e.published)) : e.url;
        if (!e.title.empty() || !e.url.empty() || !e.html.empty() || !e.guid.empty()) out.entries.push_back(std::move(e));
    }
}

void collect_opml(xmlNode* parent, const std::string& group, std::vector<Feed>& feeds) {
    for (xmlNode* n = parent->children; n; n = n->next) {
        if (!named(n, "outline")) { collect_opml(n, group, feeds); continue; }
        std::string title = prop(n, "text"); if (title.empty()) title = prop(n, "title");
        std::string next_group = group.empty() ? title : (title.empty() ? group : group + "/" + title);
        std::string url = prop(n, "xmlUrl");
        if (!url.empty()) {
            xmlURIPtr u = xmlParseURI(url.c_str());
            bool ok = u && u->scheme && u->server && (!std::strcmp(u->scheme, "http") || !std::strcmp(u->scheme, "https"));
            if (u) xmlFreeURI(u);
            if (ok && std::none_of(feeds.begin(), feeds.end(), [&](const Feed& f) { return f.url == url; })) {
                Feed f; f.url = url; f.title = prop(n, "title"); if (f.title.empty()) f.title = title; f.group = group; feeds.push_back(std::move(f));
            }
        }
        collect_opml(n, next_group, feeds);
    }
}

xmlDocPtr parse_xml(std::string_view xml, const std::string& url = {}) {
    if (xml.size() > kMaxXml) throw std::runtime_error("XML exceeds 8 MiB limit");
    xmlDocPtr doc = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_COMPACT);
    if (!doc || doc->intSubset || doc->extSubset) {
        if (doc) xmlFreeDoc(doc);
        throw std::runtime_error("invalid or unsafe XML");
    }
    if (!url.empty()) doc->URL = xmlStrdup(BAD_CAST url.c_str());
    return doc;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '&') out += "&amp;"; else if (c == '"') out += "&quot;"; else if (c == '<') out += "&lt;"; else if (c == '>') out += "&gt;"; else out += c; }
    return out;
}
}

std::vector<ArticleImage> html_images(std::string_view html,const std::string& base_url) {
    if(html.size()>kMaxXml)throw std::runtime_error("HTML 超过 8 MB 上限");
    std::vector<ArticleImage> result;if(html.empty())return result;
    std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> doc(
        htmlReadMemory(html.data(),static_cast<int>(html.size()),base_url.c_str(),"UTF-8",
                       HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING),xmlFreeDoc);
    if(!doc)return result;
    std::vector<xmlNode*> stack{xmlDocGetRootElement(doc.get())};
    while(!stack.empty()&&result.size()<20){
        auto* n=stack.back();stack.pop_back();if(!n)continue;
        if(n->next)stack.push_back(n->next);
        if(named(n,"script")||named(n,"style"))continue;
        if(named(n,"img")){
            auto raw=prop(n,"data-src");if(raw.empty())raw=prop(n,"src");
            const auto url=resolve_url(n,raw,base_url);
            if((url.rfind("http://",0)==0||url.rfind("https://",0)==0)&&url.size()<=2048&&
               url.find_first_of("\r\n")==std::string::npos&&
               std::none_of(result.begin(),result.end(),[&](const ArticleImage& image){return image.url==url;})){
                ArticleImage image;image.url=url;image.alt=prop(n,"alt");result.push_back(std::move(image));
            }
        }
        if(n->children)stack.push_back(n->children);
    }
    return result;
}

namespace {
struct TextAndAnchors { std::string text; std::vector<ImageAnchor> anchors; };

bool safe_image_url(const std::string& url) {
    return (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) &&
           url.size() <= 2048 && url.find_first_of("\r\n") == std::string::npos;
}

TextAndAnchors html_text_and_anchors(std::string_view html, const std::string& base_url, bool collect_anchors) {
    TextAndAnchors result;
    if (html.empty()) return result;
    htmlDocPtr doc = htmlReadMemory(html.data(), static_cast<int>(std::min(html.size(), kMaxXml)),
                                    base_url.empty() ? nullptr : base_url.c_str(), "UTF-8",
                                    HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT);
    if (!doc) { result.text = std::string(html.substr(0, kMaxXml)); return result; }
    std::string out;
    std::vector<std::size_t> raw_offsets;
    std::vector<std::size_t> raw_caption_ends;
    std::vector<xmlNode*> captions;
    auto walk = [&](auto&& self, xmlNode* n, bool hidden) -> void {
        for (; n && out.size() < kMaxXml; n = n->next) {
            if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
                if (!hidden && n->content) out += reinterpret_cast<const char*>(n->content);
                continue;
            }
            if (n->type != XML_ELEMENT_NODE) continue;
            std::string tag = reinterpret_cast<const char*>(n->name);
            bool hide = hidden || tag == "script" || tag == "style";
            bool block = tag == "p" || tag == "div" || tag == "br" || tag == "li" || tag == "pre" || tag == "blockquote" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "tr";
            if (!hidden && block && !out.empty() && out.back() != '\n') out += '\n';
            if (tag == "li" && !hidden) out += "• ";
            if (collect_anchors && !hide && tag == "img" && result.anchors.size() < 200) {
                auto raw = prop(n, "data-src"); if (raw.empty()) raw = prop(n, "src");
                const auto url = resolve_url(n, raw, base_url);
                if (safe_image_url(url)) {
                    xmlNode* figure = n->parent;
                    while (figure && !named(figure, "figure")) figure = figure->parent;
                    xmlNode* caption = nullptr;
                    if (figure) {
                        xmlNode* next = n->next;
                        while (next && next->type == XML_TEXT_NODE && next->content &&
                               std::strspn(reinterpret_cast<const char*>(next->content), " \t\r\n") == static_cast<std::size_t>(xmlStrlen(next->content))) next = next->next;
                        if (named(next, "figcaption") && next->parent == figure) caption = next;
                        else if (!next && n->parent != figure) {
                            next = n->parent->next;
                            while (next && next->type == XML_TEXT_NODE && next->content &&
                                   std::strspn(reinterpret_cast<const char*>(next->content), " \t\r\n") == static_cast<std::size_t>(xmlStrlen(next->content))) next = next->next;
                            if (named(next, "figcaption") && next->parent == figure) caption = next;
                        }
                    }
                    result.anchors.push_back({url, out.size(), out.size()});
                    raw_offsets.push_back(out.size()); raw_caption_ends.push_back(out.size()); captions.push_back(caption);
                }
            }
            self(self, n->children, hide);
            if (tag == "figcaption")
                for (std::size_t i = 0; i < captions.size(); ++i) if (captions[i] == n) raw_caption_ends[i] = out.size();
            if (!hidden && block && (out.empty() || out.back() != '\n')) out += '\n';
        }
    };
    walk(walk, xmlDocGetRootElement(doc), false);
    xmlFreeDoc(doc);
    std::string clean; clean.reserve(out.size());
    // Only map image/caption boundaries; a per-byte size_t map would cost 64 MiB for an 8 MiB article.
    std::vector<std::pair<std::size_t,std::size_t>> boundaries;
    for(std::size_t i=0;i<raw_offsets.size();++i){boundaries.emplace_back(raw_offsets[i],2*i);boundaries.emplace_back(raw_caption_ends[i],2*i+1);}
    std::sort(boundaries.begin(),boundaries.end());
    std::vector<std::size_t> mapped(boundaries.size());
    std::size_t boundary=0;
    auto map_boundary=[&](std::size_t offset,std::size_t clean_offset){
        while(boundary<boundaries.size()&&boundaries[boundary].first==offset){mapped[boundaries[boundary].second]=clean_offset;++boundary;}
    };
    bool space = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        map_boundary(i,clean.size() + (space ? 1 : 0));
        unsigned char c = static_cast<unsigned char>(out[i]);
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || c == ' ') {
            if (c == '\n') { while (!clean.empty() && clean.back() == ' ') clean.pop_back(); if (clean.empty() || clean.back() != '\n') clean += '\n'; space = false; }
            else if (!clean.empty() && clean.back() != '\n') space = true;
        } else { if (space && !clean.empty() && clean.back() != '\n') clean += ' '; clean += static_cast<char>(c); space = false; }
    }
    map_boundary(out.size(),clean.size() + (space ? 1 : 0));
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == ' ')) clean.pop_back();
    result.text = std::move(clean);
    for (std::size_t i = 0; i < result.anchors.size(); ++i)
    {
        result.anchors[i].offset = std::min(mapped[2*i], result.text.size());
        result.anchors[i].caption_end = captions[i] ? std::min(mapped[2*i+1], result.text.size()) : result.anchors[i].offset;
    }
    for(std::size_t i=0;i+1<result.anchors.size();++i)
        if(result.anchors[i].caption_end>result.anchors[i+1].offset)result.anchors[i].caption_end=result.anchors[i].offset;
    return result;
}
}

std::string html_to_text(std::string_view html) {
    return html_text_and_anchors(html, {}, false).text;
}

std::vector<ImageAnchor> html_image_anchors(std::string_view html, const std::string& base_url) {
    if (html.size() > kMaxXml) throw std::runtime_error("HTML 超过 8 MB 上限");
    return html_text_and_anchors(html, base_url, true).anchors;
}

ParsedFeed parse_feed(std::string_view xml, const std::string& source_url) {
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> owner(parse_xml(xml, source_url), xmlFreeDoc);
    xmlDocPtr doc = owner.get();
    xmlNode* root = xmlDocGetRootElement(doc);
    ParsedFeed result;
    bool atom = named(root, "feed") && namespace_href(root) == "http://www.w3.org/2005/Atom";
    if (!atom && !named(root, "rss") && !named(root, "RDF"))
        throw std::runtime_error("内容不是有效的 RSS / Atom 订阅");
    if (atom) {
        result.title = child_text(root, "title");
        entries(root, source_url, result, true);
    } else {
        xmlNode* channel = named(root, "channel") ? root : child(root, "channel");
        if (channel) {
            result.title = child_text(channel, "title");
            entries(channel, source_url, result, false);
            if (channel != root) entries(root, source_url, result, false);
        }
        else entries(root, source_url, result, false);
    }
    if (result.title.empty()) result.title = fallback_title(source_url);
    return result;
}

std::vector<Feed> parse_opml(std::string_view xml) {
    xmlDocPtr doc = parse_xml(xml);
    std::vector<Feed> feeds;
    xmlNode* root = xmlDocGetRootElement(doc);
    if (!named(root, "opml")) { xmlFreeDoc(doc); throw std::runtime_error("not an OPML document"); }
    collect_opml(root, {}, feeds);
    xmlFreeDoc(doc);
    return feeds;
}

std::string export_opml(const std::vector<Feed>& feeds) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\"><head><title>Kindle RSS</title></head><body>";
    std::vector<std::string> opened;
    for (const auto& f : feeds) {
        std::vector<std::string> path;
        std::istringstream groups(f.group);
        for (std::string part; std::getline(groups, part, '/');) if (!part.empty()) path.push_back(part);
        std::size_t common = 0;
        while (common < path.size() && common < opened.size() && path[common] == opened[common]) ++common;
        while (opened.size() > common) { out += "</outline>"; opened.pop_back(); }
        while (opened.size() < path.size()) { out += "<outline text=\"" + escape(path[opened.size()]) + "\">"; opened.push_back(path[opened.size()]); }
        out += "<outline type=\"rss\" text=\"" + escape(f.title) + "\" title=\"" + escape(f.title) + "\" xmlUrl=\"" + escape(f.url) + "\"/>";
    }
    while (!opened.empty()) { out += "</outline>"; opened.pop_back(); }
    return out + "</body></opml>";
}
}
