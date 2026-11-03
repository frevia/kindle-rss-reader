#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rss {
enum class ReadingMode { Original, Translated, Bilingual };
struct ReadingAnchor { std::size_t source = 0, display = 0; };
struct ArticleImage {
    std::string url,alt,error;
    int width=0,height=0;
    std::vector<std::uint8_t> pixels; // Decoded grayscale, loaded only for the displayed image.
};
struct Feed {
    std::int64_t id = 0;
    std::string url, title, group, error, etag, modified;
    std::int64_t synced_at = 0;
};
struct Entry {
    std::int64_t id = 0, feed_id = 0;
    std::string guid, title, url, author, html, text;
    std::int64_t published = 0;
    bool read = false, starred = false;
    bool in_progress = false;
    std::vector<ArticleImage> images;
    std::int64_t position = 0; // UTF-8 byte offset, independent of font size.
    bool web_content = false;
    ReadingMode reading_mode = ReadingMode::Original;
    std::vector<ReadingAnchor> reading_anchors;
};
struct FulltextStatus { std::string state, error; };
struct TranslationStatus {
    std::string state, error;
    std::size_t completed = 0, total = 0;
};
struct ParsedFeed { std::string title; std::vector<Entry> entries; };
enum class View { Unread, All, Starred, Today };
}
