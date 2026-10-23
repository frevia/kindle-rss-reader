#pragma once
#include "model.hpp"
#include <string_view>

namespace rss {
// Throws std::runtime_error for invalid or oversized input. No external entities.
ParsedFeed parse_feed(std::string_view xml, const std::string& source_url);
std::vector<Feed> parse_opml(std::string_view xml);
std::string export_opml(const std::vector<Feed>& feeds);
std::string html_to_text(std::string_view html);
struct ImageAnchor { std::string url; std::size_t offset = 0; std::size_t caption_end = 0; };
std::vector<ImageAnchor> html_image_anchors(std::string_view html, const std::string& base_url);
std::vector<ArticleImage> html_images(std::string_view html,const std::string& base_url);
}
