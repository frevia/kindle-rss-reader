#include "directory.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace rss;

static Directory make_directory(const std::string& url, DirectoryKind kind = DirectoryKind::Opml) {
    Directory d; d.title = "Local directory"; d.url = url; d.kind = kind; return d;
}

static std::string cache_key_for_url(const std::string& url) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string key = "directory_cache:";
    for (unsigned char c : url) { key.push_back(hex[c >> 4]); key.push_back(hex[c & 15]); }
    return key;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string base = argv[1], scenario = argv[2];
    const auto path = "/tmp/kindle-rss-directory-network-" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    {
        Store store(path);
        std::atomic<bool> cancel{false};
        if (scenario == "website") {
            bool failed = false; try { save_directory(store, make_directory(base + "/ok.opml", DirectoryKind::Website)); } catch (const std::runtime_error&) { failed = true; }
            assert(failed && directory_catalogs(store).size() == 2);
        } else if (scenario == "pre-cancel") {
            const auto id = save_directory(store, make_directory(base + "/ok.opml")); cancel = true;
            bool failed = false; try { refresh_directory(store, id, cancel); } catch (const std::runtime_error&) { failed = true; }
            assert(failed && directory_cache(store, id).updated_at == 0);
        } else if (scenario == "success") {
            const auto id = save_directory(store, make_directory(base + "/ok.opml"));
            refresh_directory(store, id, cancel);
            const auto cached = directory_cache(store, id);
            assert(cached.updated_at > 0 && cached.feeds.size() == 1);
            assert(cached.feeds[0].title == "First & <Second>" && cached.feeds[0].group == "Tech");
            assert(store.feeds().empty());
            Store reopened(path);
            const auto offline = directory_cache(reopened, id);
            assert(offline.updated_at == cached.updated_at && offline.feeds.size() == 1);
        } else if (scenario == "large-cache") {
            const auto id = save_directory(store, make_directory(base + "/large-cache.opml"));
            refresh_directory(store, id, cancel);
            const auto key = cache_key_for_url(base + "/large-cache.opml");
            assert(store.setting(key).size() > 256 * 1024);
            Store reopened(path);
            const auto cached = directory_cache(reopened, id);
            assert(cached.updated_at > 0 && cached.feeds.size() == 3200 && reopened.setting(key).size() > 256 * 1024);
        } else if (scenario == "preserve" || scenario == "limit") {
            const auto id = save_directory(store, make_directory(base + (scenario=="limit"?"/limit-sequence":"/switch") + ".opml"));
            refresh_directory(store, id, cancel);
            const auto cached = directory_cache(store, id);
            bool failed = false; try { refresh_directory(store, id, cancel); } catch (const std::runtime_error& e) {
                failed=scenario!="limit"||std::string(e.what()).find("5000")!=std::string::npos;
            }
            const auto after = directory_cache(store, id);
            assert(failed && after.updated_at == cached.updated_at && after.feeds.size() == cached.feeds.size());
        } else if (scenario == "cancel") {
            const auto id = save_directory(store, make_directory(base + "/cancel-sequence.opml"));
            refresh_directory(store, id, cancel);
            const auto cached = directory_cache(store, id);
            std::thread stop([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel = true; });
            bool failed = false; try { refresh_directory(store, id, cancel); } catch (const std::runtime_error&) { failed = true; }
            stop.join();
            const auto after = directory_cache(store, id);
            assert(failed && after.updated_at == cached.updated_at && after.feeds.size() == cached.feeds.size());
        } else {
            const auto id = save_directory(store, make_directory(base + "/" + scenario + ".opml"));
            bool failed = false; try { refresh_directory(store, id, cancel); } catch (const std::runtime_error&) { failed = true; }
            assert(failed && directory_cache(store, id).updated_at == 0);
        }
    }
    std::remove(path.c_str());
}
