#pragma once
#include "model.hpp"
#include "render.hpp"

namespace rss {
struct SelectionRange { std::string start, end, text; };
struct ReaderSelection { SelectionRange range; std::vector<Rect> rects; };
struct SelectionJob {
    std::int64_t entry_id=0;
    bool web=false;
    ReadingMode mode=ReadingMode::Original;
    std::string revision;
    SelectionRange range;
};
struct StoredSelection { SelectionRange range; bool highlighted=false; std::string translation; };
}
