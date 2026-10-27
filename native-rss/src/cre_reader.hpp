#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace rss {
class Store;
class Canvas;
struct Entry;
class CreReader {
public:
    static bool available();
    CreReader(Store& store, const std::string& font);
    ~CreReader();
    void open(const Entry& entry, const std::string& base_url, const std::string& metadata,
              int width, int height, int font_size, int line_gap);
    void layout(int width, int height, int font_size, int line_gap);
    std::size_t page_count() const;
    std::size_t restore(const std::string& anchor);
    std::string anchor(std::size_t page);
    void draw(Canvas& canvas, std::size_t page, int x, int y, int clip_bottom);
private:
    struct Impl;
    Store& store_;
    std::string font_;
    std::unique_ptr<Impl> impl_;
};
}
