#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include "selection.hpp"

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
    std::size_t text_position(std::size_t page);
    std::size_t page_for_text_position(std::size_t position);
    std::optional<ReaderSelection> select(std::size_t page, int x1, int y1, int x2, int y2, bool adjusting = false);
    std::vector<Rect> selection_rects(std::size_t page, const SelectionRange& range);
    void draw(Canvas& canvas, std::size_t page, int x, int y, int clip_bottom);
private:
    struct Impl;
    Store& store_;
    std::string font_;
    std::unique_ptr<Impl> impl_;
};
}
