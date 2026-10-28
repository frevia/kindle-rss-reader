#pragma once
#include "store.hpp"
#include <atomic>
#include <functional>

namespace rss {
struct Response { long status = 0; std::string body, etag, modified, effective_url, content_type, location; };
Response fetch(const std::string& url, const std::string& etag, const std::string& modified,
               const std::atomic<bool>& cancel);
struct SyncProgress { int completed = 0, total = 0, failed = 0; std::string message; };
void sync_feeds(Store& store, const std::atomic<bool>& cancel,
                const std::function<void(const SyncProgress&)>& progress, bool failed_only = false);
void sync_fulltext(Store& store, std::int64_t entry_id, const std::atomic<bool>& cancel,
                   const std::function<void(const SyncProgress&)>& progress);
}
