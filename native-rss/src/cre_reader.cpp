#include "cre_reader.hpp"
#include "feed.hpp"
#include "render.hpp"
#include "store.hpp"
#include <algorithm>
#include <stdexcept>

#ifdef RSS_CRE
#include "lvdocview.h"
#include "lvfntman.h"
#include "crhyphman.h"
#include "ldomdocument.h"
#include "lvgraydrawbuf.h"
#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <png.h>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace rss {
namespace {
constexpr std::size_t max_html = 512 * 1024, max_image_pixels = 16 * 1024 * 1024;
std::string escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}
std::string xml_string(xmlChar* value) {
    if (!value) return {};
    std::string out(reinterpret_cast<char*>(value));
    xmlFree(value);
    return out;
}
std::string revision(const std::string& document) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : document) { hash ^= c; hash *= 1099511628211ULL; }
    return std::to_string(hash); // Content identity only, never used as a security boundary.
}
void write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw std::runtime_error("新版排版临时文件写入失败");
}
struct FontEngine {
    std::string font, face;
    explicit FontEngine(const std::string& path): font(path) {
        if (!InitFontManager(lString8::empty_str)) throw std::runtime_error("新版排版字体初始化失败");
        if (!fontMan->RegisterFont(lString8(path.c_str()))) {
            ShutdownFontManager();
            throw std::runtime_error("新版排版无法读取正文字体");
        }
        lString32Collection faces;
        fontMan->getFaceList(faces);
        if (!faces.length()) { ShutdownFontManager(); throw std::runtime_error("正文字体没有可用字形"); }
        face = UnicodeToUtf8(faces[0]).c_str();
        HyphMan::initDictionaries(lString32::empty_str);
    }
    ~FontEngine() { HyphMan::uninit(); ShutdownFontManager(); }
};
FontEngine& engine(const std::string& path) {
    static FontEngine fonts(path);
    // shortcut: RSS currently has one font per process; register multiple families when a font picker is added.
    if (fonts.font != path) throw std::runtime_error("新版排版需要重启后切换字体");
    return fonts;
}
const char* base_css = R"CSS(
html,body,main,article,section,div,p,h1,h2,h3,h4,h5,h6,blockquote,pre,ul,ol,li,figure,figcaption,table,tr,td,th {display:block;}
head,script,style {display:none;}
body {margin:0;padding:0;text-align:justify;font-size:100%;}
p {margin:0 0 .35em 0;text-indent:0;orphans:2;widows:2;}
h1,h2,h3,h4,h5,h6 {font-weight:bold;line-height:1.3;margin:.6em 0 .35em 0;page-break-after:avoid;}
h1,h2,h3,h4,h5,h6 {font-size:100%;}
strong,b,em,i,u,s,del,small,sup,sub,a,code {display:inline;}
.rss-title {margin-top:0;} .rss-meta {font-size:75%;color:#555;margin-bottom:1em;}
strong,b {font-weight:bold;} em,i {font-style:italic;} a {text-decoration:underline;}
blockquote {margin:.5em 1em;padding-left:.5em;border-left:2px solid #888;}
pre {white-space:pre-wrap;margin:.5em 0;padding:.3em;line-height:1.45;}
pre,code {font-family:monospace;}
.rss-plain {white-space:pre-wrap;}
ul,ol {margin:.4em 0;padding-left:1.4em;} li {display:list-item;}
ul {list-style-type:disc;} ol {list-style-type:decimal;}
figure {margin:.5em 0;page-break-inside:avoid;}
img {display:block;max-width:100%;margin:.4em auto;}
figcaption {font-size:81.25%;line-height:1.4;text-align:center;text-indent:0;page-break-before:avoid;}
.rss-missing {font-size:85%;color:#555;}
table {display:table;max-width:100%;border-collapse:collapse;}
thead,tbody {display:table-row-group;}
tr {display:table-row;} td,th {display:table-cell;padding:.2em;border:1px solid #999;}
th {font-weight:bold;} br {display:inline;} hr {display:block;margin:.5em 0;border-top:1px solid #999;}
)CSS";
}

struct CreReader::Impl {
    std::filesystem::path directory;
    std::unique_ptr<LVDocView> view;
    std::string content_revision;
    int width = 0, height = 0, size = 0, gap = 0;
    Impl() {
        const auto session = std::getenv("RSS_CRE_CACHE");
        const auto guard = std::getenv("RSS_UI_GUARD");
        if (guard && std::string(guard) == "1" && (!session || std::string(session).rfind("/tmp/rss-cre-session.", 0) != 0))
            throw std::runtime_error("请从 RSS 阅读器入口启动");
        if (session) {
            struct stat info{};
            if (::lstat(session, &info) || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)
                throw std::runtime_error("新版排版临时目录不可用");
        }
        const auto name = session ? std::string(session) + "/article-XXXXXX" : "/tmp/rss-cre-XXXXXX";
        std::vector<char> pattern(name.begin(), name.end());pattern.push_back('\0');
        const auto path = ::mkdtemp(pattern.data());
        if (!path) throw std::runtime_error("新版排版无法建立临时目录");
        directory = path;
    }
    ~Impl() {
        view.reset();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

bool CreReader::available() { return true; }
CreReader::CreReader(Store& store, const std::string& font): store_(store), font_(font) {}
CreReader::~CreReader() = default;

void CreReader::open(const Entry& entry, const std::string& base_url, const std::string& metadata,
                     int width, int height, int font_size, int line_gap) {
    // shortcut: large documents use the original renderer; lift these limits after bounded-memory stress tests.
    if (entry.html.size() > max_html || entry.text.size() > max_html || entry.title.size() > 16384 || metadata.size() > 16384 || base_url.size() > 8192)
        throw std::runtime_error("文章过大，请使用原排版");
    auto next = std::make_unique<Impl>();
    std::map<std::string, std::string> image_files;
    std::size_t pixel_budget = 0, png_budget = 0, image_nodes = 0;
    const auto image_markup = [&](xmlNode* node) -> std::string {
        if (++image_nodes > 200) throw std::runtime_error("文章图片过多，请使用原排版");
        auto raw = xml_string(xmlGetProp(node, BAD_CAST "data-src"));
        if (raw.empty()) raw = xml_string(xmlGetProp(node, BAD_CAST "src"));
        const auto missing = "<p class=\"rss-missing\">[图片未缓存：" + escape(xml_string(xmlGetProp(node, BAD_CAST "alt"))) + "]</p>";
        if (raw.empty()) return missing;
        auto base = xml_string(xmlNodeGetBase(node->doc, node));
        if (base.empty()) base = base_url;
        const auto resolved = xml_string(xmlBuildURI(BAD_CAST raw.c_str(), base.empty() ? nullptr : BAD_CAST base.c_str()));
        const auto url = resolved.empty() ? raw : resolved;
        const auto reference = std::find_if(entry.images.begin(), entry.images.end(), [&](const auto& image) { return image.url == url; });
        if (reference == entry.images.end()) return missing;
        auto file = image_files.find(url);
        if (file == image_files.end()) {
            const auto cached = store_.image(entry.id, url);
            if (!cached || !cached->error.empty() || cached->pixels.empty()) return missing;
            if (cached->width <= 0 || cached->height <= 0 || cached->width > 1072 || cached->height > 1448 ||
                cached->pixels.size() != static_cast<std::size_t>(cached->width) * cached->height)
                throw std::runtime_error("文章图片缓存无效，请使用原排版");
            pixel_budget += cached->pixels.size();
            if (pixel_budget > max_image_pixels) throw std::runtime_error("文章图片过大，请使用原排版");
            const auto name = "image-" + std::to_string(image_files.size()) + ".png";
            png_image png{};
            png.version = PNG_IMAGE_VERSION;
            png.width = cached->width;
            png.height = cached->height;
            png.format = PNG_FORMAT_GRAY;
            const auto path = next->directory / name;
            const bool written = png_image_write_to_file(&png, path.c_str(), 0, cached->pixels.data(), 0, nullptr);
            png_image_free(&png);
            if (!written) throw std::runtime_error("新版排版图片写入失败");
            png_budget += std::filesystem::file_size(path);
            if (png_budget > max_image_pixels) throw std::runtime_error("文章图片过大，请使用原排版");
            file = image_files.emplace(url, name).first;
        }
        return "<img src=\"" + file->second + "\" alt=\"" + escape(reference->alt) + "\"/>";
    };
    std::string body;
    if (entry.html.empty() || (!entry.text.empty() && html_to_text(entry.html) != entry.text))
        body = "<div class=\"rss-plain\">" + escape(entry.text) + "</div>";
    else {
        auto doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>(
            htmlReadMemory(entry.html.data(), static_cast<int>(entry.html.size()), base_url.empty() ? nullptr : base_url.c_str(), "UTF-8",
                           HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT), xmlFreeDoc);
        if (!doc) throw std::runtime_error("文章正文无法解析，请使用原排版");
        const std::set<std::string> allowed{"p","div","section","article","main","h1","h2","h3","h4","h5","h6","blockquote","pre","code","strong","b","em","i","u","s","del","small","sup","sub","ul","ol","li","figure","figcaption","table","thead","tbody","tr","td","th","a","br","hr"};
        const std::set<std::string> hidden{"head","script","style","iframe","object","embed","svg","math","video","audio","form","input","textarea","template","noscript","link","meta","base"};
        std::size_t nodes = 0;
        std::function<void(xmlNode*, int)> walk = [&](xmlNode* start, int depth) {
            if (depth > 64) throw std::runtime_error("文章结构过深，请使用原排版");
            for (auto node = start; node; node = node->next) {
                if (++nodes > 10000) throw std::runtime_error("文章结构过大，请使用原排版");
                if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
                    if (node->content) body += escape(reinterpret_cast<char*>(node->content));
                } else if (node->type == XML_ELEMENT_NODE) {
                    const std::string tag(reinterpret_cast<const char*>(node->name));
                    if (hidden.count(tag)) continue;
                    if (tag == "img") { body += image_markup(node); continue; }
                    const bool keep = allowed.count(tag);
                    if (keep) body += "<" + tag + ">";
                    walk(node->children, depth + 1);
                    if (keep && tag != "br" && tag != "hr") body += "</" + tag + ">";
                }
                if (body.size() > max_html * 6) throw std::runtime_error("文章过大，请使用原排版");
            }
        };
        walk(xmlDocGetRootElement(doc.get()), 0);
    }
    const auto document = "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\"/></head><body><h1 class=\"rss-title\">" +
        escape(entry.title) + "</h1><p class=\"rss-meta\">" + escape(metadata) + "</p>" + body + "</body></html>";
    next->content_revision = revision(document);
    write_file(next->directory / "article.html", document);
    auto& fonts = engine(font_);
    next->view = std::make_unique<LVDocView>(8, true);
    next->view->setViewMode(DVM_PAGES, 1);
    next->view->setPageHeaderInfo(0);
    next->view->setPageMargins(lvRect(0, 0, 0, 0));
    next->view->setDefaultFontFace(lString8(fonts.face.c_str()));
    next->view->setBackgroundColor(0xffffff);
    next->view->setTextColor(0x141414);
    next->view->setDefaultInterlineSpace(100);
    auto old = std::move(impl_);
    impl_ = std::move(next);
    try {
        layout(width, height, font_size, line_gap);
        if (!impl_->view->LoadDocument((impl_->directory / "article.html").c_str()))
            throw std::runtime_error("新版排版无法打开文章");
        impl_->view->Render();
        if (page_count() == 0 || page_count() > 512) throw std::runtime_error("文章分页过多，请使用原排版");
    } catch (...) { impl_ = std::move(old); throw; }
}

void CreReader::layout(int width, int height, int font_size, int line_gap) {
    if (!impl_) throw std::runtime_error("尚未打开新版排版");
    if (width < 100 || height < 100 || width > 4096 || height > 4096 || font_size < 24 || font_size > 48 || line_gap < 10 || line_gap > 42)
        throw std::runtime_error("新版排版参数无效");
    auto& value = *impl_;
    if (value.width == width && value.height == height && value.size == font_size && value.gap == line_gap) return;
    std::ostringstream css;
    css << base_css << "body {line-height:" << (100.0 * (font_size + line_gap) / font_size) << "%;}"
        << "h1,.rss-title {font-size:" << (100.0*std::min(48,font_size+6)/font_size) << "%;}"
        << "h2 {font-size:" << (100.0*std::min(48,font_size+3)/font_size) << "%;}";
    value.view->Resize(width, height);
    value.view->setFontSize(font_size);
    value.view->setStyleSheet(lString8(css.str().c_str()));
    value.width = width; value.height = height; value.size = font_size; value.gap = line_gap;
    if (value.view->isDocumentOpened()) {
        value.view->Render();
        if (page_count() > 512) throw std::runtime_error("文章分页过多，请使用原排版");
    }
}
std::size_t CreReader::page_count() const { return impl_ ? static_cast<std::size_t>(impl_->view->getPageCount()) : 0; }
std::size_t CreReader::restore(const std::string& anchor) {
    if (!impl_) throw std::runtime_error("尚未打开新版排版");
    auto& value = *impl_;
    const auto prefix = value.content_revision + ":";
    // shortcut: changed content restarts experimental reading; add text-anchor migration before making it the default.
    if (anchor.size() <= 4096 && anchor.rfind(prefix, 0) == 0) {
        const auto pointer = value.view->getDocument()->createXPointer(Utf8ToUnicode(anchor.substr(prefix.size()).c_str()));
        if (!pointer.isNull()) { value.view->goToBookmark(pointer); return value.view->getCurPage(); }
    }
    value.view->goToPage(0);
    return 0;
}
std::string CreReader::anchor(std::size_t page) {
    if (!impl_ || page >= page_count()) throw std::runtime_error("新版排版页码无效");
    impl_->view->goToPage(static_cast<int>(page));
    return impl_->content_revision + ":" + UnicodeToUtf8(impl_->view->getBookmark().toString()).c_str();
}
void CreReader::draw(Canvas& canvas, std::size_t page, int x, int y, int clip_bottom) {
    if (!impl_ || page >= page_count() || x < 0 || y < 0 || x + impl_->width > canvas.width || y + impl_->height > canvas.height)
        throw std::runtime_error("新版排版绘制范围无效");
    impl_->view->goToPage(static_cast<int>(page));
    LVGrayDrawBuf buffer(impl_->width, impl_->height, 8);
    buffer.Clear(0xffffff);
    impl_->view->Draw(buffer);
    for (int row = 0; row < std::clamp(clip_bottom - y, 0, impl_->height); ++row)
        std::copy_n(buffer.GetScanLine(row), impl_->width, canvas.pixels.begin() + static_cast<std::size_t>(y + row) * canvas.width + x);
}
}
#else
namespace rss {
struct CreReader::Impl {};
bool CreReader::available() { return false; }
CreReader::CreReader(Store& store, const std::string& font): store_(store), font_(font) {}
CreReader::~CreReader() = default;
void CreReader::open(const Entry&, const std::string&, const std::string&, int, int, int, int) { throw std::runtime_error("此构建未启用新版排版"); }
void CreReader::layout(int, int, int, int) { throw std::runtime_error("此构建未启用新版排版"); }
std::size_t CreReader::page_count() const { return 0; }
std::size_t CreReader::restore(const std::string&) { return 0; }
std::string CreReader::anchor(std::size_t) { return {}; }
void CreReader::draw(Canvas&, std::size_t, int, int, int) { throw std::runtime_error("此构建未启用新版排版"); }
}
#endif
