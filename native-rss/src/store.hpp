#pragma once
#include "model.hpp"
#include <optional>
#include <sqlite3.h>

namespace rss {
class Store {
public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::int64_t add_feed(const Feed& feed); // URL dedup; retain existing custom name/group.
    std::vector<Feed> feeds() const;
    std::vector<std::string> groups() const;
    void add_group(const std::string& path);
    void rename_group(const std::string& old_path, const std::string& new_path);
    void remove_group(const std::string& path);
    void edit_feed(std::int64_t id, const std::string& title, const std::string& group);
    void update_feed(const Feed& feed);
    void save_entries(std::int64_t feed_id, const ParsedFeed& parsed); // Transaction, preserves state.
    void sync_result(std::int64_t feed_id, const std::string& error,
                     const std::string& etag, const std::string& modified, std::int64_t now);
    // List metadata only; use entry(id) to load one article body.
    std::vector<Entry> entries(View view, std::int64_t feed_id = 0,
                               const std::string& search = "", std::int64_t today_start = 0,
                               const std::optional<std::string>& group = std::nullopt) const;
    std::optional<Entry> entry(std::int64_t id, bool with_content = true) const;
    std::optional<ArticleImage> image(std::int64_t entry_id,const std::string& url) const;
    void save_image(std::int64_t entry_id,const ArticleImage& image);
    std::vector<std::pair<std::int64_t,ArticleImage>> pending_images() const;
    void set_read(std::int64_t id, bool value);
    void set_starred(std::int64_t id, bool value);
    void set_position(std::int64_t id, std::int64_t offset);
    void finish_reading(std::int64_t id);
    void mark_feed_read(std::int64_t feed_id); // 0 = all.
    void remove_feed(std::int64_t id); // Keep starred entries and their source metadata.
    std::size_t clean_read(std::int64_t before); // Preserve starred and in-progress entries.
    void set_setting(const std::string& key, const std::string& value);
    std::string setting(const std::string& key, const std::string& fallback = "") const;
private:
    sqlite3* db_ = nullptr;
};
}
