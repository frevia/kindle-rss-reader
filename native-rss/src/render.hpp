#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <utility>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace rss {
enum class Icon { None, Rss, Search, Sync, Star, Back, Next, Close, Book, Clock, Inbox, Folder, Plus, Settings, Image, Download, Upload, Exit, Check, StarFilled, Info, Battery, Wifi, BackCircle };
struct Rect { int x, y, w, h; bool contains(int px, int py) const; };
struct Line { std::string text; std::size_t start = 0, end = 0; };
struct InlineImage { std::size_t offset=0,caption_end=0; int index=0,width=0,height=0; };
struct PageElement { Line line; Rect rect{}; int image=-1, occurrence=-1, size=0; bool caption=false; };
struct Page { std::vector<Line> lines; std::size_t start = 0, end = 0; std::vector<PageElement> elements; int image_anchor=-1; };
struct Glyph { int width=0, height=0, left=0, top=0, advance=0; std::vector<std::uint8_t> pixels; };
class Canvas {
public:
    Canvas(int width, int height, const std::string& font);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    int width, height;
    const std::string& font_path() const { return font_path_; }
    std::vector<std::uint8_t> pixels;
    void clear(std::uint8_t color = 255);
    void fill(Rect r, std::uint8_t color);
    void icon(Icon kind, Rect area, std::uint8_t color=30);
    void rule(int x, int y, int w, std::uint8_t color = 210);
    int text(int x, int baseline, const std::string& value, int size, std::uint8_t color=20);
    int measure(const std::string& value, int size);
    std::pair<int,int> text_vertical_bounds(const std::string& value, int size);
    std::vector<Line> wrap(const std::string& value, int size, int max_width);
    std::vector<Page> paginate(const std::string& value, int size, int line_height,
                               int content_width, int content_height, int first_height = 0);
    std::vector<Page> paginate_article(const std::string& value,const std::vector<InlineImage>& images,
                                      int size,int line_height,int content_width,int content_height,int first_height);
    std::string ellipsize(const std::string& value, int size, int max_width);
    void write_pgm(const std::string& path) const;
private:
    std::string font_path_;
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    std::map<std::pair<int, std::uint32_t>, Glyph> cache_;
    std::map<std::tuple<Icon, int, int>, std::vector<std::uint16_t>> icon_coverage_cache_;
    std::size_t icon_coverage_entries_ = 0;
    const Glyph& glyph(std::uint32_t codepoint, int size);
};
std::size_t page_at(const std::vector<Page>& pages, std::size_t offset);
}
