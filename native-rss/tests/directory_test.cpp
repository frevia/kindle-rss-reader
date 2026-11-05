#include "directory.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

using namespace rss;

template <class Action>
static bool rejected(Action&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    assert(same_feed_url("https://EXAMPLE.com:443/feed#old","https://example.com/feed"));
    assert(same_feed_url("http://example.com:80/rss","http://example.com/rss"));
    assert(!same_feed_url("http://example.com/feed","https://example.com/feed"));
    assert(!same_feed_url("https://example.com/feed?a=1","https://example.com/feed?a=2"));
    assert(!same_feed_url("https://example.com/Feed","https://example.com/feed"));
    assert(same_feed_url("https://user:secret@example.com/rss","https://user:secret@example.com/rss"));
    assert(!same_feed_url("https://user:secret@example.com/rss","https://example.com/rss"));
    const auto path = "/tmp/kindle-rss-directory-" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    std::string id;
    const auto cache_key = [](const std::string& url) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string key = "directory_cache:";
        for (unsigned char c : url) { key.push_back(hex[c >> 4]); key.push_back(hex[c & 15]); }
        return key;
    };
    {
        Store store(path);
        auto catalogs = directory_catalogs(store);
        assert(catalogs.size() == 2);
        assert(catalogs[0].id == "blogs" && catalogs[0].kind == DirectoryKind::Opml);
        assert(directory_cache(store, "blogs").feeds.empty() && directory_cache(store, "blogs").updated_at == 0);
        assert(catalogs[1].id == "routes" && catalogs[1].kind == DirectoryKind::Opml);
        assert(rejected([&] { Directory blank; blank.title="   "; blank.url="https://example.net/blank.opml"; save_directory(store,blank); }));

        Directory custom; custom.title = "研究 & <标签> \"中文\""; custom.url = "HTTP://Example.COM:80/list.opml";
        id = save_directory(store, custom);
        assert(!id.empty());
        assert(rejected([&] { Directory duplicate; duplicate.title = "dup"; duplicate.url = "http://example.com/list.opml"; save_directory(store, duplicate); }));
        for (const auto& bad : {"https://user:pass@example.com/x.opml", "file:///tmp/x.opml", "https://example.com/a b.opml", "https://example.com/\nfeed.opml",
                                "https://example.com", "https://example.com/feed", "https://example.com/feed.xml",
                                "https://example.com/feed.opml/", "https://example.com/feed.opml.xml", "https://example.com/feed.opml?x=1",
                                "https://example.com/feed.opml#part", "https://example.com/feed.opml?", "https://example.com/feed.opml#",
                                "https://example.com/?name=feed.opml", "https://example.com/#feed.opml"})
            assert(rejected([&] { Directory value; value.title = "bad"; value.url = bad; save_directory(store, value); }));
        for (const auto& good : {"http://example.net/list.opml", "https://example.net/LIST.OPML"})
            assert(!save_directory(store, Directory{"", "accepted", good, DirectoryKind::Opml, false}).empty());
        assert(rejected([&] { Directory value; value.id = "blogs"; value.title = "x"; value.url = "https://example.net/readonly.opml"; save_directory(store, value); }));
        Directory website; website.title = "website"; website.kind = DirectoryKind::Website; website.url = "https://example.net/site";
        assert(rejected([&] { save_directory(store, website); }));

        const auto before_edit = directory_catalogs(store);
        auto edited = *std::find_if(before_edit.begin(), before_edit.end(), [&](const Directory& d) { return d.id == id; });
        edited.url = "https://new.example.org/catalog.opml";
        edited.title = "改名 & < > \"' 中";
        assert(save_directory(store, edited) == id);
        const auto edited_catalogs = directory_catalogs(store);
        const auto saved = *std::find_if(edited_catalogs.begin(), edited_catalogs.end(), [&](const Directory& d) { return d.id == id; });
        assert(saved.id == id && saved.title == edited.title && saved.url == "https://new.example.org/catalog.opml");
        assert(directory_cache(store, id).feeds.empty());

        store.set_setting(cache_key(saved.url), "cache-bytes");
        const auto metadata_before_invalid_edits = store.setting("directory_catalogs_v1");
        const auto cache_before_invalid_edits = store.setting(cache_key(saved.url));
        for (const auto& bad : {"https://new.example.org/catalog", "https://new.example.org/catalog.opml?x=1", "https://new.example.org/catalog.opml#frag"}) {
            auto invalid_edit = saved;
            invalid_edit.url = bad;
            assert(rejected([&] { save_directory(store, invalid_edit); }));
            assert(store.setting("directory_catalogs_v1") == metadata_before_invalid_edits);
            assert(store.setting(cache_key(saved.url)) == cache_before_invalid_edits);
        }
        const auto metadata_before_invalid_create = store.setting("directory_catalogs_v1");
        assert(rejected([&] { Directory invalid; invalid.title = "invalid"; invalid.url = "https://new.example.org/not-opml"; save_directory(store, invalid); }));
        assert(store.setting("directory_catalogs_v1") == metadata_before_invalid_create);
        assert(store.setting(cache_key(saved.url)) == cache_before_invalid_edits);

        Feed subscription; subscription.url = "https://kept.example/feed"; subscription.title = "Kept";
        const auto feed_id = store.add_feed(subscription);
        Entry entry; entry.guid = "keep"; entry.title = "Keep article";
        ParsedFeed content; content.entries.push_back(entry); store.save_entries(feed_id, content);
        remove_directory(store, id);
        assert(directory_catalogs(store).size() == 4);
        assert(store.feeds().size() == 1 && store.entry_count(View::All, feed_id) == 1);
        assert(rejected([&] { remove_directory(store, "blogs"); }));
    }
    {
        Store reopened(path);
        const std::string legacy = "<directories><directory id=\"legacy-opml\" title=\"Legacy\" url=\"https://legacy.example/old-feed\" kind=\"opml\"/><directory id=\"legacy-website\" title=\"Legacy website\" url=\"https://legacy.example/#/rss?tag=a&amp;b\" kind=\"website\"/></directories>";
        reopened.set_setting("directory_catalogs_v1", legacy);
        Directory value; value.title = "Persisted"; value.url = "https://persist.example/opml.opml";
        const auto persisted = save_directory(reopened, value);
        Store again(path);
        const auto catalogs = directory_catalogs(again);
        assert(catalogs.size() == 5 && catalogs[2].id == "legacy-opml" && catalogs[2].url == "https://legacy.example/old-feed");
        assert(catalogs[3].id == "legacy-website" && catalogs[3].kind == DirectoryKind::Website && catalogs[3].url == "https://legacy.example/#/rss?tag=a&b");
        assert(catalogs.back().id == persisted);
        auto changed = catalogs.back(); changed.url = "https://updated.example/opml.opml";
        save_directory(again, changed);
        assert(directory_cache(again, persisted).updated_at == 0);
        assert(directory_catalogs(again)[2].id == "legacy-opml" && directory_catalogs(again)[3].id == "legacy-website");
    }
    std::remove(path.c_str());
}
