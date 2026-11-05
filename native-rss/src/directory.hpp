#pragma once

#include "feed.hpp"
#include "store.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rss {
enum class DirectoryKind { Opml, Website };
struct Directory { std::string id,title,url; DirectoryKind kind=DirectoryKind::Opml; bool builtin=false; };
struct DirectoryCache { std::vector<Feed> feeds; std::int64_t updated_at=0; };

// Compare normalized HTTP(S) addresses without rewriting legacy subscriptions.
bool same_feed_url(const std::string& left,const std::string& right);
std::vector<Directory> directory_catalogs(const Store&);
std::string save_directory(Store&,const Directory&);
void remove_directory(Store&,const std::string& id);
DirectoryCache directory_cache(const Store&,const std::string& id);
void refresh_directory(Store&,const std::string& id,const std::atomic<bool>& cancel);
}
