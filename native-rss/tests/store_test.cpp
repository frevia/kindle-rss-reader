#include "store.hpp"

#include <cassert>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace rss;

static Entry make_entry(std::string guid, std::string title, std::int64_t published = 0) {
    Entry entry;
    entry.guid = std::move(guid);
    entry.title = std::move(title);
    entry.url = "https://example.test/article";
    entry.published = published;
    return entry;
}

int main() {
    const std::string path = "/tmp/kindle-rss-store-test-" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    std::int64_t feed_id = 0;
    std::int64_t first_id = 0;
    std::int64_t old_published = 0;
    {
        Store store(path);
        Feed feed; feed.url = "https://example.test/feed"; feed.title = "Imported name"; feed.group = "news";
        feed_id = store.add_feed(feed);
        feed.title = "Custom name"; feed.group = "custom";
        assert(store.add_feed(feed) == feed_id);
        assert(store.feeds().size() == 1);
        assert(store.feeds()[0].title == "Imported name");
        assert(store.feeds()[0].group == "news");
        struct stat file_info {};
        assert(::stat(path.c_str(), &file_info) == 0);
        assert((file_info.st_mode & 0777) == 0600);

        ParsedFeed parsed;
        parsed.entries = {make_entry("a", "Percent % literal", 100), make_entry("b", "Underscore _ and \xc3\xa9", 200)};
        store.save_entries(feed_id, parsed);
        auto entries = store.entries(View::All);
        assert(entries.size() == 2 && entries[0].guid == "b");
        first_id = entries[1].id;
        old_published = entries[1].published;
        store.set_read(first_id, true);
        store.set_starred(first_id, true);
        store.set_position(first_id, 42);
        store.set_position(entries[0].id, -5);
        assert(store.entry(entries[0].id)->position == 0);

        ParsedFeed refreshed; refreshed.entries = {make_entry("a", "Percent % literal updated"), make_entry("b", "Updated _ title \xc3\xa9")};
        store.save_entries(feed_id, refreshed);
        auto retained = store.entry(first_id);
        assert(retained && retained->read && retained->starred && retained->position == 42);
        assert(retained->published == old_published);
        assert(store.entries(View::Unread).size() == 1);
        assert(store.entries(View::Today, 0, "", 150).size() == 1);
        assert(store.entries(View::All, feed_id, "%").size() == 1);
        assert(store.entries(View::All, feed_id, "_").size() == 1);
        assert(store.entries(View::All, feed_id, "\xc3\xa9").size() == 1);
        assert(store.entries(View::All, 0, "%_").empty());

        store.sync_result(feed_id, "", "tag-one", "date-one", 500);
        store.sync_result(feed_id, "network error", "tag-bad", "date-bad", 600);
        assert(store.feeds()[0].error == "network error");
        assert(store.feeds()[0].etag == "tag-one" && store.feeds()[0].modified == "date-one");
        assert(store.feeds()[0].synced_at == 500);

        store.set_read(entries[0].id, true);
        store.set_position(entries[0].id, 1);
        assert(store.clean_read(1000) == 0);
        store.set_position(entries[0].id, 0);
        assert(store.clean_read(1000) == 0); // First-page reading is still in progress.
        store.finish_reading(entries[0].id);
        assert(store.clean_read(1000) == 1);
        store.remove_feed(feed_id);
        assert(store.feeds().empty());
        assert(store.entry(first_id) && store.entry(first_id)->starred);
        Store background(path);
        ParsedFeed late; late.entries = {make_entry("late", "must not resurrect", 300)};
        background.save_entries(feed_id, late);
        assert(store.entries(View::All).size() == 1);
        assert(store.entries(View::All)[0].id == first_id);
    }
    {
        Store reopened(path);
        assert(reopened.entry(first_id) && reopened.entry(first_id)->position == 42);
        Feed feed; feed.url = "https://example.test/feed"; feed.title = "Again";
        assert(reopened.add_feed(feed) == feed_id);
        assert(reopened.feeds().size() == 1);
        assert(reopened.feeds()[0].title == "Imported name");
        assert(reopened.setting("missing", "fallback") == "fallback");
        reopened.set_setting("font", "large");
        reopened.set_setting("font", "small");
        assert(reopened.setting("font") == "small");
        Feed named_later; named_later.url = "https://example.test/new"; named_later.title = named_later.url;
        auto named_id = reopened.add_feed(named_later);
        ParsedFeed named; named.title = "真实订阅标题"; reopened.save_entries(named_id, named);
        assert(reopened.feeds()[1].title == "真实订阅标题");
        sqlite3* db = nullptr;
        assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TRIGGER reject_bad BEFORE INSERT ON entries WHEN NEW.guid='bad' BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        bool failed = false;
        ParsedFeed invalid; invalid.entries = {make_entry("good", "will roll back"), make_entry("bad", "trigger failure")};
        try { reopened.save_entries(feed_id, invalid); } catch (const std::runtime_error&) { failed = true; }
        assert(failed);
        assert(reopened.entries(View::All, feed_id, "will roll back").empty());
        assert(reopened.entries(View::All, feed_id, "trigger failure").empty());
    }
    std::remove(path.c_str());
    {
        Store images(":memory:");Feed f;f.url="https://images.test/feed";auto id=images.add_feed(f);
        ParsedFeed p;auto e=make_entry("image-story","Images",100);
        e.text=std::string(2*1024*1024,'x');e.html="<p>large article</p>";
        ArticleImage ref;ref.url="https://images.test/photo.png";ref.alt="original";e.images={ref};p.entries={e};images.save_entries(id,p);
        auto story=images.entries(View::All)[0].id;
        images.set_read(story,true);images.set_starred(story,true);
        auto metadata=images.entry(story,false);
        assert(metadata&&metadata->read&&metadata->starred&&metadata->title=="Images");
        assert(metadata->text.empty()&&metadata->html.empty()&&metadata->images.empty());
        assert(images.entry(story)->text.size()==2*1024*1024);
        images.set_starred(story,false);
        assert(images.pending_images().size()==1&&images.entry(story)->images.size()==1);
        ref.width=2;ref.height=1;ref.pixels={0,255};images.save_image(story,ref);
        assert(images.pending_images().empty());assert(images.image(story,ref.url)->pixels==ref.pixels);
        p.entries[0].images[0].alt="new caption";images.save_entries(id,p);
        assert(images.image(story,ref.url)->alt=="new caption");assert(images.image(story,ref.url)->pixels==ref.pixels);
        ArticleImage failure;failure.url=ref.url;failure.error="network";images.save_image(story,failure);
        assert(images.image(story,ref.url)->pixels==ref.pixels);
        images.remove_feed(id);assert(!images.image(story,ref.url));
        images.save_image(story,ref);assert(!images.image(story,ref.url)); // Late download cannot resurrect deleted content.
    }
    {
        Store groups(":memory:");
        Feed f;f.url="https://example.test/child";f.title="Child";f.group="科技/编程";
        auto child=groups.add_feed(f);
        ParsedFeed content;content.entries={make_entry("child", "匹配", 100)};groups.save_entries(child,content);
        f.url="https://example.test/other";f.group="科技新闻";auto other=groups.add_feed(f);
        groups.save_entries(other,content);
        assert(groups.entries(View::All,0,"",0,std::string("科技")).size()==1);
        assert(groups.entries(View::All,0,"匹配",0,std::string("科技/编程")).size()==1);
        assert(groups.entries(View::All,0,"",0,std::string("%")).empty());
        groups.edit_feed(child,"自定义名称","");
        assert(groups.feeds()[0].title=="自定义名称");
        assert(groups.entries(View::All,0,"",0,std::string("科技")).empty());
        assert(groups.entries(View::All,0,"",0,std::string("")).size()==1);
        content.title="服务器名称";groups.save_entries(child,content);
        assert(groups.feeds()[0].title=="自定义名称");
        bool rejected=false;try{groups.edit_feed(child,"","");}catch(const std::runtime_error&){rejected=true;}
        assert(rejected);assert(groups.feeds()[0].title=="自定义名称");
        groups.set_starred(groups.entries(View::All,child)[0].id,true);
        groups.remove_feed(child);
        assert(groups.entries(View::All,0,"",0,std::string("")).empty());
    }
}
