#include "feed.hpp"
#include "store.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>
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

static void assert_count_matches(const Store& store, View view, std::int64_t feed_id = 0,
                                 const std::string& search = "", std::int64_t today_start = 0,
                                 const std::optional<std::string>& group = std::nullopt) {
    assert(store.entry_count(view, feed_id, search, today_start, group) ==
           static_cast<std::int64_t>(store.entries(view, feed_id, search, today_start, group).size()));
}

int main() {
    {
        Store legacy(":memory:");Feed f;f.url="https://legacy.example/feed";f.title="旧分组";f.group="科技/编程";auto id=legacy.add_feed(f);
        bool duplicate=false;try{legacy.add_group("科技");}catch(const std::runtime_error&){duplicate=true;}assert(duplicate);
        legacy.rename_group("科技","技术");assert(legacy.feeds()[0].group=="技术/编程");
        legacy.remove_group("技术/编程");auto names=legacy.groups();assert(std::find(names.begin(),names.end(),"技术")!=names.end());assert(legacy.feeds()[0].id==id&&legacy.feeds()[0].group.empty());
    }

    {
        Store limited(":memory:");limited.add_group("A/"+std::string(1022,'b'));
        bool rejected=false;try{limited.rename_group("A","Longer");}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&limited.groups().back().size()==1024);
        Feed bad;bad.url="file:///tmp/feed";rejected=false;try{limited.add_feed(bad);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&limited.feeds().empty());
    }

    const std::string path = "/tmp/kindle-rss-store-test-" + std::to_string(getpid()) + ".sqlite";
    std::remove(path.c_str());
    {
        Store opening(path);Feed feed;feed.url="https://atomic-open.test/rss";const auto fid=opening.add_feed(feed);
        auto article=make_entry("atomic","Atomic opening",1);article.text="RSS body";
        ParsedFeed parsed;parsed.entries={article};opening.save_entries(fid,parsed);
        const auto id=opening.entries(View::All).front().id;
        assert(opening.queue_fulltext(id));
        auto web=article;web.html="<p>Web body</p>";web.text="Web body";opening.save_fulltext(id,web);
        sqlite3* audit=nullptr;assert(sqlite3_open(path.c_str(),&audit)==SQLITE_OK);
        assert(sqlite3_exec(audit,"CREATE TABLE deferred_open_failure(feed_id INTEGER REFERENCES feeds(id) DEFERRABLE INITIALLY DEFERRED)",nullptr,nullptr,nullptr)==SQLITE_OK);
        for(const bool fail_commit:{false,true}){
            opening.set_read(id,false);opening.set_position(id,11);opening.set_position(id,17,true);
            opening.set_setting("reader_content:"+std::to_string(id),"rss");opening.set_setting("last_entry","previous");
            const char* trigger=fail_commit?
                "CREATE TRIGGER fail_open BEFORE INSERT ON settings WHEN NEW.key='last_entry' BEGIN INSERT INTO deferred_open_failure VALUES(-999); END;":
                "CREATE TRIGGER fail_open BEFORE UPDATE OF in_progress ON entries BEGIN SELECT RAISE(ABORT,'database or disk is full'); END;";
            assert(sqlite3_exec(audit,trigger,nullptr,nullptr,nullptr)==SQLITE_OK);
            bool failed=false;try{opening.begin_reading(id,99,true);}catch(const std::runtime_error&){failed=true;}
            assert(failed&&!opening.entry(id)->read&&opening.entry(id)->in_progress);
            assert(opening.entry(id)->position==11&&opening.fulltext_entry(id)->position==17);
            assert(opening.setting("reader_content:"+std::to_string(id))=="rss"&&opening.setting("last_entry")=="previous");
            assert(opening.entry_count(View::Unread)==1);
            assert(sqlite3_exec(audit,"DROP TRIGGER fail_open",nullptr,nullptr,nullptr)==SQLITE_OK);
            opening.begin_reading(id,99,true); // A failed transaction must not poison a later open.
            assert(opening.entry(id)->read&&opening.entry(id)->in_progress&&opening.fulltext_entry(id)->position==99);
            assert(opening.entry(id)->position==11);
            assert(opening.setting("reader_content:"+std::to_string(id))=="web"&&opening.setting("last_entry")==std::to_string(id));
        }
        sqlite3_close(audit);
    }
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
        store.add_group("persisted/empty");

        ParsedFeed parsed;
        parsed.entries = {make_entry("a", "Percent % literal", 100), make_entry("b", "Underscore _ and \\ and \xc3\xa9", 200)};
        store.save_entries(feed_id, parsed);
        auto entries = store.entries(View::All);
        assert(entries.size() == 2 && entries[0].guid == "b");
        assert_count_matches(store, View::All);
        assert_count_matches(store, View::Unread);
        assert_count_matches(store, View::Starred);
        assert_count_matches(store, View::Today, 0, "", 150);
        assert_count_matches(store, View::All, feed_id);
        assert_count_matches(store, View::All, feed_id, "%");
        assert_count_matches(store, View::All, feed_id, "_");
        assert_count_matches(store, View::All, feed_id, "\\");
        assert(store.entry_count(View::All) == 2 && store.entry_count(View::Unread) == 2);
        first_id = entries[1].id;
        old_published = entries[1].published;
        store.set_read(first_id, true);
        assert(store.entry_count(View::All) == 2 && store.entry_count(View::Unread) == 1);
        store.set_starred(first_id, true);
        store.set_position(first_id, 42);
        store.set_position(entries[0].id, -5);
        assert(store.entry(entries[0].id)->position == 0);

        ParsedFeed refreshed; refreshed.entries = {make_entry("a", "Percent % literal updated"), make_entry("b", "Updated _ title \\ and \xc3\xa9")};
        store.save_entries(feed_id, refreshed);
        auto retained = store.entry(first_id);
        assert(retained && retained->read && retained->starred && retained->position == 42);
        assert(retained->published == old_published);
        assert(store.entries(View::Unread).size() == 1);
        assert(store.entries(View::Today, 0, "", 150).size() == 1);
        assert(store.entries(View::All, feed_id, "%").size() == 1);
        assert(store.entries(View::All, feed_id, "_").size() == 1);
        assert(store.entries(View::All, feed_id, "\xc3\xa9").size() == 1);
        assert(store.entries(View::All, feed_id, "\\").size() == 1);
        assert(store.entries(View::All, 0, "%_").empty());
        assert_count_matches(store, View::Unread);
        assert_count_matches(store, View::Today, 0, "", 150);
        assert_count_matches(store, View::All, feed_id, "\\");

        store.sync_result(feed_id, "", "tag-one", "date-one", 500);
        store.sync_result(feed_id, "network error", "tag-bad", "date-bad", 600);
        assert(store.feeds()[0].error == "network error");
        assert(store.feeds()[0].etag == "tag-one" && store.feeds()[0].modified == "date-one");
        assert(store.feeds()[0].synced_at == 500);

        store.set_read(entries[0].id, true);
        assert(store.entry_count(View::All) == 2 && store.entry_count(View::Unread) == 0);
        store.set_position(entries[0].id, 1);
        assert(store.clean_read(1000) == 0);
        store.set_position(entries[0].id, 0);
        assert(store.clean_read(1000) == 0); // First-page reading is still in progress.
        store.finish_reading(entries[0].id);
        assert(store.clean_read(1000) == 1);
        store.remove_feed(feed_id);
        assert(store.feeds().empty());
        assert(store.entry(first_id) && store.entry(first_id)->starred);
        assert_count_matches(store, View::All);
        assert_count_matches(store, View::Starred);
        assert(store.entry_count(View::All) == 1 && store.entry_count(View::Starred) == 1);
        Store background(path);
        ParsedFeed late; late.entries = {make_entry("late", "must not resurrect", 300)};
        background.save_entries(feed_id, late);
        assert(store.entries(View::All).size() == 1);
        assert(store.entries(View::All)[0].id == first_id);
    }
    {
        Store reopened(path);
        const auto persisted_groups=reopened.groups();
        assert(std::find(persisted_groups.begin(),persisted_groups.end(),"persisted/empty")!=persisted_groups.end());
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
        Feed unnamed; unnamed.url = "https://example.test/unnamed";
        auto unnamed_id = reopened.add_feed(unnamed);
        ParsedFeed discovered; discovered.title = "自动发现名称"; reopened.save_entries(unnamed_id, discovered);
        assert(reopened.feeds()[2].title == "自动发现名称");
        sqlite3* db = nullptr;
        assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TRIGGER reject_bad BEFORE INSERT ON entries WHEN NEW.guid='bad' BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TRIGGER reject_group BEFORE INSERT ON feed_groups WHEN NEW.path='broken' BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TRIGGER reject_rename BEFORE UPDATE OF group_name ON feeds WHEN NEW.group_name='Rollback' BEGIN SELECT RAISE(ABORT,'test failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        bool failed = false;
        ParsedFeed invalid; invalid.entries = {make_entry("good", "will roll back"), make_entry("bad", "trigger failure")};
        try { reopened.save_entries(feed_id, invalid); } catch (const std::runtime_error&) { failed = true; }
        assert(failed);
        assert(reopened.entries(View::All, feed_id, "will roll back").empty());
        assert(reopened.entries(View::All, feed_id, "trigger failure").empty());
        bool group_failed=false; try { reopened.add_group("broken/child"); } catch(const std::runtime_error&) { group_failed=true; }
        const auto after_failed_add=reopened.groups();
        assert(group_failed && std::find(after_failed_add.begin(),after_failed_add.end(),"broken")==after_failed_add.end());
        group_failed=false; try { reopened.rename_group("news","Rollback"); } catch(const std::runtime_error&) { group_failed=true; }
        assert(group_failed && reopened.feeds()[0].group=="news");
    }
    std::remove(path.c_str());
    {
        // Unchanged RSS image references must not rewrite cached pixel blobs during background sync.
        Store images(path);Feed feed;feed.url="https://image-reuse.test/rss";const auto fid=images.add_feed(feed);
        auto article=make_entry("reuse","Cached images",1);ArticleImage a,b;
        a.url="https://image-reuse.test/a.png";b.url="https://image-reuse.test/b.png";article.images={a,b};
        ParsedFeed parsed;parsed.entries={article};images.save_entries(fid,parsed);
        const auto id=images.entries(View::All).front().id;
        for(auto ref:{a,b}){ref.width=2;ref.height=1;ref.pixels={17,239};images.save_image(id,ref);}
        sqlite3* audit=nullptr;assert(sqlite3_open(path.c_str(),&audit)==SQLITE_OK);
        assert(sqlite3_exec(audit,"CREATE TABLE image_writes(n INTEGER); INSERT INTO image_writes VALUES(0); CREATE TRIGGER image_write AFTER UPDATE ON article_images BEGIN UPDATE image_writes SET n=n+1; END",nullptr,nullptr,nullptr)==SQLITE_OK);
        const auto writes=[&]{sqlite3_stmt* q=nullptr;assert(sqlite3_prepare_v2(audit,"SELECT n FROM image_writes",-1,&q,nullptr)==SQLITE_OK);assert(sqlite3_step(q)==SQLITE_ROW);const int count=sqlite3_column_int(q,0);sqlite3_finalize(q);return count;};
        images.save_entries(fid,parsed);assert(writes()==0);
        std::reverse(parsed.entries[0].images.begin(),parsed.entries[0].images.end());images.save_entries(fid,parsed);
        assert(images.entry(id)->images.front().url==b.url&&writes()==2);
        assert(images.image(id,a.url)->pixels==std::vector<std::uint8_t>({17,239}));
        parsed.entries[0].images.pop_back();images.save_entries(fid,parsed);
        assert(!images.image(id,a.url)&&images.image(id,b.url)->pixels==std::vector<std::uint8_t>({17,239}));
        sqlite3_close(audit);
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
        assert_count_matches(groups, View::All, 0, "", 0, std::string("科技"));
        assert_count_matches(groups, View::All, 0, "匹配", 0, std::string("科技/编程"));
        assert(groups.entries(View::All,0,"",0,std::string("%")).empty());
        groups.edit_feed(child,"自定义名称","");
        assert(groups.feeds()[0].title=="自定义名称");
        assert(groups.entries(View::All,0,"",0,std::string("科技")).empty());
        assert(groups.entries(View::All,0,"",0,std::string("")).size()==1);
        assert_count_matches(groups, View::All, 0, "", 0, std::string(""));
        content.title="服务器名称";groups.save_entries(child,content);
        assert(groups.feeds()[0].title=="自定义名称");
        const auto entry_id=groups.entries(View::All,child)[0].id;
        groups.set_read(entry_id,true); groups.set_starred(entry_id,true); groups.set_position(entry_id,37);
        groups.sync_result(child,"","edit-tag","edit-date",901);
        groups.edit_feed(child,"","");
        auto cleared=groups.feeds()[0];
        assert(cleared.title.empty() && cleared.etag.empty() && cleared.modified.empty() && cleared.synced_at==901);
        content.title="再次发现名称"; groups.save_entries(child,content);
        assert(groups.feeds()[0].title=="再次发现名称");
        assert(groups.entry(entry_id)->read && groups.entry(entry_id)->starred && groups.entry(entry_id)->position==37);
        bool rejected=false;try{groups.edit_feed(child,std::string(513,'x'),"");}catch(const std::runtime_error&){rejected=true;}
        assert(rejected); rejected=false; try{groups.edit_feed(child,"two\nlines","");}catch(const std::runtime_error&){rejected=true;}
        assert(rejected && groups.feeds()[0].title=="再次发现名称");
        groups.remove_feed(child);
        assert(groups.entries(View::All,0,"",0,std::string("")).empty());
    }
    {
        Store groups(":memory:");
        groups.add_group("A/empty"); groups.add_group("A/empty/deeper");
        bool rejected=false; try { groups.add_group("A//bad"); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected); rejected=false; try { groups.add_group("A/empty"); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected);
        Feed f; f.url="https://groups.test/feed"; f.title="State"; f.group="A/sub"; auto id=groups.add_feed(f);
        ParsedFeed parsed; parsed.entries={make_entry("saved","saved",100)}; groups.save_entries(id,parsed);
        const auto entry_id=groups.entries(View::All,id)[0].id;
        groups.set_read(entry_id,true); groups.set_starred(entry_id,true); groups.set_position(entry_id,37);
        Feed sibling; sibling.url="https://groups.test/sibling"; sibling.group="AB/child"; groups.add_feed(sibling);
        Feed wildcard; wildcard.url="https://groups.test/wildcard"; wildcard.group="%_/child"; groups.add_feed(wildcard);
        groups.add_group("Target");
        rejected=false; try { groups.rename_group("A","Target"); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected && groups.feeds()[0].group=="A/sub");
        rejected=false; try { groups.rename_group("A","A/sub/inside"); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected && groups.feeds()[0].group=="A/sub");
        groups.rename_group("A","Moved");
        assert(groups.feeds()[0].group=="Moved/sub"); assert(groups.feeds()[1].group=="AB/child");
        assert(groups.entry(entry_id)->read && groups.entry(entry_id)->starred && groups.entry(entry_id)->position==37);
        groups.add_group("%_/literal"); groups.rename_group("%_","percent");
        assert(groups.feeds()[2].group=="percent/child");
        auto listed=groups.groups(); assert(std::find(listed.begin(),listed.end(),"percent/literal")!=listed.end());
        groups.remove_group("Moved");
        assert(groups.feeds()[0].group.empty()); assert(groups.entry(entry_id)->starred);
        groups.add_group("parent/child"); groups.remove_group("parent/child");
        const auto paths=groups.groups();
        assert(std::find(paths.begin(),paths.end(),"parent")!=paths.end()); // Parent persists after its last child is removed.
    }
    {
        Store moved(":memory:");
        moved.add_group("A/B/C/empty");
        moved.add_group("A/B/empty");
        Feed nested; nested.url="https://move.test/feed"; nested.title="Nested"; nested.group="A/B/C";
        const auto feed_id=moved.add_feed(nested);
        Entry story=make_entry("story","Preserved state",100);ArticleImage reference;reference.url="https://move.test/image.png";story.images={reference};
        ParsedFeed parsed;parsed.entries={story};moved.save_entries(feed_id,parsed);
        const auto entry_id=moved.entries(View::All,feed_id).front().id;
        moved.set_read(entry_id,true);moved.set_starred(entry_id,true);moved.set_position(entry_id,73);
        ArticleImage cached;cached.url=reference.url;cached.width=2;cached.height=1;cached.pixels={17,239};moved.save_image(entry_id,cached);
        Feed lookalike;lookalike.url="https://move.test/lookalike";lookalike.group="AB/child";moved.add_feed(lookalike);

        moved.rename_group("A/B","Target/B");
        auto paths=moved.groups();
        assert(std::find(paths.begin(),paths.end(),"A")!=paths.end());
        assert(std::find(paths.begin(),paths.end(),"Target/B/C/empty")!=paths.end());
        assert(std::find(paths.begin(),paths.end(),"Target/B/empty")!=paths.end());
        assert(std::find(paths.begin(),paths.end(),"AB/child")!=paths.end());
        assert(moved.feeds()[0].group=="Target/B/C"&&moved.feeds()[1].group=="AB/child");
        assert(moved.entries(View::All,0,"",0,std::string("Target")).size()==1);
        assert(moved.entries(View::All,0,"",0,std::string("A")).empty());

        moved.rename_group("Target/B","B");
        assert(moved.feeds()[0].group=="B/C");
        assert(moved.entries(View::All,0,"",0,std::string("B")).size()==1);
        assert(moved.entries(View::All,0,"",0,std::string("Target")).empty());
        const auto preserved=moved.entry(entry_id);const auto image=moved.image(entry_id,reference.url);
        assert(preserved&&preserved->read&&preserved->starred&&preserved->position==73);
        assert(image&&image->pixels==cached.pixels&&image->width==2&&image->height==1);

        moved.add_group("Conflict/B/child");
        const auto before=moved.groups();
        const auto rejected_move=[&](const std::string& target){bool rejected=false;try{moved.rename_group("B",target);}catch(const std::runtime_error&){rejected=true;}return rejected;};
        assert(rejected_move("B/C/inside"));
        assert(rejected_move("Conflict"));
        assert(moved.groups()==before&&moved.feeds()[0].group=="B/C");
        assert(moved.entry(entry_id)->read&&moved.entry(entry_id)->starred&&moved.entry(entry_id)->position==73);

        std::vector<std::string> imported_groups;
        const auto imported=parse_opml(export_opml(moved.feeds(),moved.groups()),&imported_groups);
        assert(std::find(imported_groups.begin(),imported_groups.end(),"B/C/empty")!=imported_groups.end());
        assert(std::find(imported_groups.begin(),imported_groups.end(),"B/C")!=imported_groups.end());
        assert(std::find(imported_groups.begin(),imported_groups.end(),"B/empty")!=imported_groups.end());
        assert(imported.size()==2);
        const auto restored=std::find_if(imported.begin(),imported.end(),[&](const Feed& feed){return feed.url==nested.url;});
        assert(restored!=imported.end()&&restored->group=="B/C");
    }

    {
        Store updates(":memory:"); Feed first; first.url="https://updates.test/old"; first.title="Before"; first.group="news";
        const auto id=updates.add_feed(first); Feed second; second.url="https://updates.test/taken"; second.title="Other"; updates.add_feed(second);
        ParsedFeed parsed; parsed.entries={make_entry("keep","kept",123)}; updates.save_entries(id,parsed);
        auto entry_id=updates.entries(View::All,id)[0].id; updates.set_read(entry_id,true); updates.set_starred(entry_id,true); updates.set_position(entry_id,37);
        updates.sync_result(id,"","tag","date",456);
        first.id=id; first.title=""; updates.update_feed(first);
        auto cleared=updates.feeds()[0];
        assert(cleared.title.empty() && cleared.etag.empty() && cleared.modified.empty() && cleared.synced_at==456);
        assert(updates.entry(entry_id)->read && updates.entry(entry_id)->starred && updates.entry(entry_id)->position==37);
        parsed.title="Source title"; updates.save_entries(id,parsed);
        assert(updates.feeds()[0].title=="Source title" && updates.feeds()[0].synced_at==456);
        first.title="Custom title"; updates.update_feed(first);
        parsed.title="Later source title"; updates.save_entries(id,parsed);
        assert(updates.feeds()[0].title=="Custom title");
        updates.sync_result(id,"","edit-tag","edit-date",789);
        updates.edit_feed(id,"",first.group);
        cleared=updates.feeds()[0];
        assert(cleared.title.empty() && cleared.etag.empty() && cleared.modified.empty() && cleared.synced_at==789);
        parsed.title="Edit source title"; updates.save_entries(id,parsed);
        assert(updates.feeds()[0].title=="Edit source title" && updates.feeds()[0].synced_at==789);
        first.title=std::string(513,'x'); bool failed=false;
        try { updates.update_feed(first); } catch(const std::runtime_error&) { failed=true; }
        assert(failed && updates.feeds()[0].title=="Edit source title");
        first.title="two\nlines"; failed=false; try { updates.update_feed(first); } catch(const std::runtime_error&) { failed=true; }
        assert(failed && updates.feeds()[0].title=="Edit source title");
        updates.sync_result(id,"old error","tag","date",999);
        first.id=id; first.url="https://updates.test/new"; first.title="After"; first.group="new/group"; updates.update_feed(first);
        auto changed=updates.feeds()[0]; assert(changed.id==id && changed.url==first.url && changed.title=="After" && changed.group=="new/group");
        assert(changed.error=="old error" && changed.etag.empty() && changed.modified.empty() && changed.synced_at==0);
        assert(updates.entry(entry_id)->read && updates.entry(entry_id)->starred && updates.entry(entry_id)->position==37 && updates.entries(View::All,id).size()==1);
        first.url="https://updates.test/taken"; first.title="Must remain"; failed=false;
        try { updates.update_feed(first); } catch(const std::runtime_error&) { failed=true; }
        assert(failed && updates.feeds()[0].url=="https://updates.test/new" && updates.feeds()[0].title=="After");
        first.url="https://updates.test/new"; first.group="invalid//group"; failed=false;
        try { updates.update_feed(first); } catch(const std::runtime_error&) { failed=true; }
        assert(failed && updates.feeds()[0].group=="new/group");
        first.group="new/group";
        first.url="file:///tmp/x"; failed=false; try { updates.update_feed(first); } catch(const std::runtime_error&) { failed=true; }
        assert(failed && updates.feeds()[0].url=="https://updates.test/new");
    }
    {
        Store reuse(":memory:");
        Feed current; current.url="https://reuse.test/current"; current.title="Current name"; current.group="news";
        const auto current_id=reuse.add_feed(current);
        ParsedFeed parsed; auto article=make_entry("keep","kept",123);
        ArticleImage image; image.url="https://reuse.test/image.png"; article.images={image}; parsed.entries={article};
        reuse.save_entries(current_id,parsed);
        const auto entry_id=reuse.entries(View::All,current_id)[0].id;
        reuse.set_read(entry_id,true); reuse.set_starred(entry_id,true); reuse.set_position(entry_id,37);
        image.width=1; image.height=1; image.pixels={42}; reuse.save_image(entry_id,image);
        Feed abandoned; abandoned.url="https://reuse.test/empty"; abandoned.title="Abandoned";
        const auto abandoned_id=reuse.add_feed(abandoned); reuse.remove_feed(abandoned_id);
        reuse.sync_result(current_id,"","old-tag","old-date",456);
        current.id=current_id; current.url=abandoned.url; reuse.update_feed(current);
        auto active=reuse.feeds();
        assert(active.size()==1 && active[0].id==current_id && active[0].url==abandoned.url);
        assert(active[0].title=="Current name" && active[0].group=="news");
        assert(active[0].etag.empty() && active[0].modified.empty() && active[0].synced_at==0);
        auto kept=reuse.entry(entry_id);
        assert(kept && kept->read && kept->starred && kept->position==37 && kept->images.size()==1);
        assert(reuse.image(entry_id,image.url)->pixels==image.pixels);
        assert(reuse.add_feed(abandoned)==current_id);
        current.url=abandoned.url; current.title="Renamed"; current.group="archive"; reuse.update_feed(current);
        assert(reuse.feeds()[0].id==current_id && reuse.feeds()[0].title=="Renamed" && reuse.feeds()[0].group=="archive");
    }
    {
        Store duplicates(":memory:");
        Feed current; current.url="https://duplicate.test/current"; current.title="Current";
        const auto current_id=duplicates.add_feed(current);
        Feed occupied; occupied.url="https://duplicate.test/occupied"; occupied.title="Occupied";
        const auto occupied_id=duplicates.add_feed(occupied);
        current.id=current_id; current.url=occupied.url;
        bool failed=false; std::string error;
        try { duplicates.update_feed(current); } catch(const std::runtime_error& e) { failed=true; error=e.what(); }
        assert(failed && error.find("其他订阅")!=std::string::npos);
        const auto active=duplicates.feeds();
        assert(active.size()==2 && active[0].id==current_id && active[0].url=="https://duplicate.test/current");
        assert(active[1].id==occupied_id && active[1].url==occupied.url && active[1].title=="Occupied");
    }
    {
        Store history(":memory:");
        Feed current; current.url="https://history.test/current"; current.title="Current";
        const auto current_id=history.add_feed(current);
        Feed archived; archived.url="https://history.test/archived"; archived.title="Archived";
        const auto archived_id=history.add_feed(archived);
        ParsedFeed parsed; parsed.entries={make_entry("starred","saved",321)}; history.save_entries(archived_id,parsed);
        const auto starred_id=history.entries(View::All,archived_id)[0].id;
        history.set_starred(starred_id,true); history.set_position(starred_id,51); history.remove_feed(archived_id);
        current.id=current_id; current.url=archived.url;
        bool failed=false; std::string error;
        try { history.update_feed(current); } catch(const std::runtime_error& e) { failed=true; error=e.what(); }
        assert(failed && error.find("收藏")!=std::string::npos && error.find("恢复")!=std::string::npos);
        assert(history.feeds().size()==1 && history.feeds()[0].id==current_id && history.feeds()[0].url=="https://history.test/current");
        const auto saved=history.entry(starred_id);
        assert(saved && saved->starred && saved->position==51 && saved->title=="saved");
    }
    {
        Store rollback(":memory:");
        Feed historical; historical.url="https://rollback.test/historical"; historical.title="Historical";
        const auto historical_id=rollback.add_feed(historical); rollback.remove_feed(historical_id);
        Feed missing; missing.id=999999; missing.url=historical.url; missing.title="Missing source";
        bool failed=false; try { rollback.update_feed(missing); } catch(const std::runtime_error&) { failed=true; }
        assert(failed);
        assert(rollback.add_feed(historical)==historical_id);
        assert(rollback.feeds().size()==1 && rollback.feeds()[0].id==historical_id && rollback.feeds()[0].title=="Historical");
    }
    {
        Store store(":memory:");Feed feed;feed.url="https://fulltext.test/feed";const auto feed_id=store.add_feed(feed);
        ParsedFeed historical;historical.entries={make_entry("before","Before",50)};store.save_entries(feed_id,historical);
        store.set_setting("feed_fulltext:"+std::to_string(feed_id),"1");
        ParsedFeed parsed;auto rss=make_entry("one","Original",100);rss.html="<p>RSS</p>";rss.text="RSS text";ArticleImage rss_image;rss_image.url="https://fulltext.test/rss.png";rss.images={rss_image};parsed.entries={rss};store.save_entries(feed_id,parsed);
        const auto initial_entries=store.entries(View::All);const auto initial=std::find_if(initial_entries.begin(),initial_entries.end(),[](const Entry& e){return e.guid=="one";});assert(initial!=initial_entries.end());const auto id=initial->id;
        assert(store.pending_fulltext().size()==1);assert(store.pending_fulltext()[0].html.empty());
        Entry full;full.url="https://fulltext.test/final";full.html="<article>full</article>";full.text="Full text";
        ArticleImage full_image;full_image.url="https://fulltext.test/full.png";full_image.alt="selected";full.images={full_image};
        store.save_fulltext(id,full);
        ArticleImage cache=full_image;cache.width=1;cache.height=1;cache.pixels={42};store.save_image(id,cache);
        assert(store.fulltext_status(id).state=="ready"&&!store.queue_fulltext(id));
        auto full_entry=store.fulltext_entry(id);assert(full_entry&&full_entry->web_content&&full_entry->url==full.url&&full_entry->text==full.text&&full_entry->images.size()==1&&full_entry->images[0].alt=="selected");
        auto original=store.entry(id);assert(original&&original->html=="<p>RSS</p>"&&original->text=="RSS text"&&original->images.size()==1);
        store.set_position(id,19,true);store.set_position(id,7);full_entry=store.fulltext_entry(id);original=store.entry(id);
        assert(full_entry&&full_entry->position==19&&original&&original->position==7&&original->in_progress);
        ParsedFeed refreshed;refreshed.entries={make_entry("one","Updated RSS",100)};store.save_entries(feed_id,refreshed);
        assert(store.image(id,full_image.url)&&store.image(id,full_image.url)->pixels==cache.pixels);
        assert(store.entry(id)->images.empty()&&store.fulltext_entry(id)->images.size()==1);
        store.finish_reading(id);assert(store.fulltext_entry(id)->position==0&&!store.entry(id)->in_progress);

        auto failed_article=make_entry("two","Failed",200);failed_article.html="<p>original</p>";failed_article.text="original text";ParsedFeed second;second.entries={failed_article};store.save_entries(feed_id,second);
        const auto all_entries=store.entries(View::All);const auto failed_id=std::find_if(all_entries.begin(),all_entries.end(),[](const Entry& e){return e.guid=="two";});
        // The feed setting queues new articles automatically; disabling it pauses automatic work.
        assert(failed_id!=all_entries.end());const auto two=failed_id->id;
        store.set_setting("feed_fulltext:"+std::to_string(feed_id),"0");assert(store.pending_fulltext().empty());
        store.fail_fulltext(two,"fetch failed");assert(store.fulltext_status(two).state=="failed"&&store.fulltext_status(two).error=="fetch failed");
        assert(store.pending_fulltext(true).empty());assert(store.queue_fulltext(two));assert(store.fulltext_status(two).state=="pending"&&store.pending_fulltext().size()==1);
        Entry invalid=full;invalid.text.clear();bool rejected=false;try{store.save_fulltext(two,invalid);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&store.fulltext_status(two).state=="pending"&&store.entry(two)->text=="original text");
        auto cascade_article=make_entry("three","Cascade",300);ParsedFeed third;third.entries={cascade_article};store.save_entries(feed_id,third);
        const auto before_remove=store.entries(View::All);const auto cascade=std::find_if(before_remove.begin(),before_remove.end(),[](const Entry& e){return e.guid=="three";});assert(cascade!=before_remove.end());const auto cascade_id=cascade->id;assert(store.queue_fulltext(cascade_id));
        store.set_starred(two,true);store.set_starred(id,true);store.remove_feed(feed_id);assert(!store.queue_fulltext(two));store.save_fulltext(two,full);
        assert(store.fulltext_status(two).state=="pending"&&!store.fulltext_entry(two));
        assert(store.queue_fulltext(id)==false); // Ready fulltext survives unsubscribe but cannot be refreshed.
        assert(store.fulltext_status(cascade_id).state.empty());
    }
    {
        const std::string legacy_path="/tmp/kindle-rss-fulltext-migration-"+std::to_string(getpid())+".sqlite";std::remove(legacy_path.c_str());
        sqlite3* db=nullptr;assert(sqlite3_open(legacy_path.c_str(),&db)==SQLITE_OK);
        assert(sqlite3_exec(db,"CREATE TABLE feeds(id INTEGER PRIMARY KEY,url TEXT NOT NULL UNIQUE,title TEXT NOT NULL DEFAULT '',group_name TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',etag TEXT NOT NULL DEFAULT '',modified TEXT NOT NULL DEFAULT '',synced_at INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1); CREATE TABLE entries(id INTEGER PRIMARY KEY,feed_id INTEGER NOT NULL REFERENCES feeds(id),guid TEXT NOT NULL,title TEXT NOT NULL DEFAULT '',url TEXT NOT NULL DEFAULT '',author TEXT NOT NULL DEFAULT '',html TEXT NOT NULL DEFAULT '',text TEXT NOT NULL DEFAULT '',published INTEGER NOT NULL DEFAULT 0,read INTEGER NOT NULL DEFAULT 0,starred INTEGER NOT NULL DEFAULT 0,position INTEGER NOT NULL DEFAULT 0,UNIQUE(feed_id,guid)); INSERT INTO feeds(id,url) VALUES(1,'https://migration.test/feed'); INSERT INTO entries(id,feed_id,guid,title,url,text,position) VALUES(2,1,'old','Old','https://migration.test/old','retained',12);",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
        {Store migrated(legacy_path);assert(migrated.entry(2)&&migrated.entry(2)->text=="retained");assert(migrated.fulltext_status(2).state.empty());assert(migrated.queue_fulltext(2));}
        {Store reopened(legacy_path);assert(reopened.entry(2)&&reopened.fulltext_status(2).state=="pending");reopened.set_starred(2,true);reopened.remove_feed(1);assert(reopened.entry(2)&&reopened.queue_fulltext(2)==false);}
        std::remove(legacy_path.c_str());
    }
    {
        const auto path="/tmp/kindle-rss-fulltext-concurrent-"+std::to_string(getpid())+".sqlite";
        std::remove(path.c_str());
        {
            Store store(path);Feed feed;feed.url="https://concurrent.test/rss";const auto fid=store.add_feed(feed);
            ParsedFeed parsed;parsed.entries={make_entry("one","Concurrent queue",1)};store.save_entries(fid,parsed);
            const auto id=store.entries(View::All).front().id;Store second(path),third(path);
            std::atomic<int> queued{0};
            std::thread a([&]{if(second.queue_fulltext(id))++queued;});
            std::thread b([&]{if(third.queue_fulltext(id))++queued;});
            a.join();b.join();assert(queued==1&&store.pending_fulltext().size()==1);
        }
        std::remove(path.c_str());
    }
}
