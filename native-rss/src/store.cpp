#include "store.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace rss {
namespace {
void check(sqlite3* db, int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) { check(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr)); }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
    void text(int index, const std::string& value) { check(db_, sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT)); }
    void integer(int index, std::int64_t value) { check(db_, sqlite3_bind_int64(stmt_, index, value)); }
    void boolean(int index, bool value) { check(db_, sqlite3_bind_int(stmt_, index, value ? 1 : 0)); }
    void blob(int index,const std::vector<std::uint8_t>& value) {
        check(db_,sqlite3_bind_blob(stmt_,index,value.empty()?"":static_cast<const void*>(value.data()),static_cast<int>(value.size()),SQLITE_TRANSIENT));
    }
    void run() { check(db_, sqlite3_step(stmt_)); }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        const std::string error = message ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}

std::string column_text(sqlite3_stmt* stmt, int col) {
    const auto* value = sqlite3_column_text(stmt, col);
    return value ? reinterpret_cast<const char*>(value) : "";
}

Feed read_feed(sqlite3_stmt* stmt) {
    Feed feed;
    feed.id = sqlite3_column_int64(stmt, 0);
    feed.url = column_text(stmt, 1);
    feed.title = column_text(stmt, 2);
    feed.group = column_text(stmt, 3);
    feed.error = column_text(stmt, 4);
    feed.etag = column_text(stmt, 5);
    feed.modified = column_text(stmt, 6);
    feed.synced_at = sqlite3_column_int64(stmt, 7);
    return feed;
}

Entry read_entry(sqlite3_stmt* stmt) {
    Entry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    entry.feed_id = sqlite3_column_int64(stmt, 1);
    entry.guid = column_text(stmt, 2);
    entry.title = column_text(stmt, 3);
    entry.url = column_text(stmt, 4);
    entry.author = column_text(stmt, 5);
    entry.html = column_text(stmt, 6);
    entry.text = column_text(stmt, 7);
    entry.published = sqlite3_column_int64(stmt, 8);
    entry.read = sqlite3_column_int(stmt, 9) != 0;
    entry.starred = sqlite3_column_int(stmt, 10) != 0;
    entry.position = sqlite3_column_int64(stmt, 11);
    entry.in_progress = sqlite3_column_int(stmt, 12) != 0;
    return entry;
}

const char* entry_columns = "id,feed_id,guid,title,url,author,html,text,published,read,starred,position,in_progress";
const char* metadata_columns = "id,feed_id,guid,title,url,author,'' AS html,'' AS text,published,read,starred,position,in_progress";
const char* feed_columns = "id,url,title,group_name,error,etag,modified,synced_at";
}

Store::Store(const std::string& path) {
    const bool existed = path == ":memory:" || std::filesystem::exists(path);
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        const std::string error = db_ ? sqlite3_errmsg(db_) : "unable to open database";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(error);
    }
    try {
        if (!existed && path != ":memory:" && chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
            throw std::runtime_error("unable to restrict database permissions");
        check(db_, sqlite3_busy_timeout(db_, 5000));
        exec(db_, "PRAGMA foreign_keys=ON");
        exec(db_, "CREATE TABLE IF NOT EXISTS feeds ("
                  "id INTEGER PRIMARY KEY, url TEXT NOT NULL UNIQUE, title TEXT NOT NULL DEFAULT '',"
                  "group_name TEXT NOT NULL DEFAULT '', error TEXT NOT NULL DEFAULT '', etag TEXT NOT NULL DEFAULT '',"
                  "modified TEXT NOT NULL DEFAULT '', synced_at INTEGER NOT NULL DEFAULT 0, active INTEGER NOT NULL DEFAULT 1)");
        exec(db_, "CREATE TABLE IF NOT EXISTS entries ("
                  "id INTEGER PRIMARY KEY, feed_id INTEGER NOT NULL REFERENCES feeds(id), guid TEXT NOT NULL,"
                  "title TEXT NOT NULL DEFAULT '', url TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '',"
                  "html TEXT NOT NULL DEFAULT '', text TEXT NOT NULL DEFAULT '', published INTEGER NOT NULL DEFAULT 0,"
                  "read INTEGER NOT NULL DEFAULT 0, starred INTEGER NOT NULL DEFAULT 0, position INTEGER NOT NULL DEFAULT 0,"
                  "UNIQUE(feed_id,guid))");
        exec(db_, "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
        exec(db_, "CREATE TABLE IF NOT EXISTS article_images (entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
                  "url TEXT NOT NULL,alt TEXT NOT NULL DEFAULT '',ordinal INTEGER NOT NULL,error TEXT NOT NULL DEFAULT '',"
                  "width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,pixels BLOB NOT NULL DEFAULT X'',"
                  "PRIMARY KEY(entry_id,url))");
        exec(db_, "CREATE INDEX IF NOT EXISTS entry_order ON entries(published DESC,id DESC)");
        bool has_progress = false;
        {
            Statement schema(db_, "PRAGMA table_info(entries)");
            int step;
            while ((step = sqlite3_step(schema.get())) == SQLITE_ROW)
                if (column_text(schema.get(), 1) == "in_progress") has_progress = true;
            check(db_, step);
        }
        if (!has_progress) {
            exec(db_, "BEGIN IMMEDIATE");
            try {
                exec(db_, "ALTER TABLE entries ADD COLUMN in_progress INTEGER NOT NULL DEFAULT 0");
                exec(db_, "UPDATE entries SET in_progress=1 WHERE position>0");
                exec(db_, "COMMIT");
            } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Store::~Store() { if (db_) sqlite3_close(db_); }

std::int64_t Store::add_feed(const Feed& feed) {
    Statement insert(db_, "INSERT INTO feeds(url,title,group_name) VALUES(?,?,?) ON CONFLICT(url) DO UPDATE SET active=1");
    insert.text(1, feed.url); insert.text(2, feed.title); insert.text(3, feed.group); insert.run();
    Statement select(db_, "SELECT id FROM feeds WHERE url=?");
    select.text(1, feed.url);
    check(db_, sqlite3_step(select.get()));
    return sqlite3_column_int64(select.get(), 0);
}

void Store::edit_feed(std::int64_t id, const std::string& title, const std::string& group) {
    if (title.empty() || title.size()>512 || group.size()>1024 ||
        title.find_first_of("\r\n\t")!=std::string::npos || group.find_first_of("\r\n\t")!=std::string::npos)
        throw std::runtime_error("名称不能为空，名称和分组必须为单行且长度适中");
    Statement q(db_, "UPDATE feeds SET title=?,group_name=? WHERE id=? AND active=1");
    q.text(1,title); q.text(2,group); q.integer(3,id); q.run();
    if (!sqlite3_changes(db_)) throw std::runtime_error("订阅已不存在");
}

std::vector<Feed> Store::feeds() const {
    Statement query(db_, (std::string("SELECT ") + feed_columns + " FROM feeds WHERE active=1 ORDER BY id").c_str());
    std::vector<Feed> result;
    while (sqlite3_step(query.get()) == SQLITE_ROW) result.push_back(read_feed(query.get()));
    check(db_, sqlite3_errcode(db_));
    return result;
}

void Store::save_entries(std::int64_t feed_id, const ParsedFeed& parsed) {
    exec(db_, "BEGIN IMMEDIATE");
    try {
        // A download started before unsubscribe must not restore removed articles.
        Statement active(db_, "SELECT active FROM feeds WHERE id=?");
        active.integer(1, feed_id);
        const int state = sqlite3_step(active.get());
        check(db_, state);
        if (state == SQLITE_DONE) throw std::runtime_error("订阅不存在");
        if (!sqlite3_column_int(active.get(), 0)) { exec(db_, "COMMIT"); return; }
        Statement save(db_, "INSERT INTO entries(feed_id,guid,title,url,author,html,text,published) VALUES(?,?,?,?,?,?,?,CASE WHEN ?=0 THEN CAST(strftime('%s','now') AS INTEGER) ELSE ? END) "
                            "ON CONFLICT(feed_id,guid) DO UPDATE SET title=excluded.title,url=excluded.url,author=excluded.author,"
                            "html=excluded.html,text=excluded.text,published=CASE WHEN ?=0 THEN entries.published ELSE excluded.published END");
        for (const auto& entry : parsed.entries) {
            check(db_, sqlite3_reset(save.get()));
            check(db_, sqlite3_clear_bindings(save.get()));
            save.integer(1, feed_id); save.text(2, entry.guid); save.text(3, entry.title); save.text(4, entry.url);
            save.text(5, entry.author); save.text(6, entry.html); save.text(7, entry.text);
            save.integer(8, entry.published); save.integer(9, entry.published); save.integer(10, entry.published);
            save.run();
            Statement lookup(db_,"SELECT id FROM entries WHERE feed_id=? AND guid=?");
            lookup.integer(1,feed_id);lookup.text(2,entry.guid);check(db_,sqlite3_step(lookup.get()));
            const auto id=sqlite3_column_int64(lookup.get(),0);
            Statement old(db_,"UPDATE article_images SET ordinal=-1 WHERE entry_id=?");old.integer(1,id);old.run();
            int ordinal=0;
            for(const auto& image:entry.images){
                if(ordinal==20)break;
                Statement ref(db_,"INSERT INTO article_images(entry_id,url,alt,ordinal) VALUES(?,?,?,?) "
                                  "ON CONFLICT(entry_id,url) DO UPDATE SET alt=excluded.alt,ordinal=excluded.ordinal");
                ref.integer(1,id);ref.text(2,image.url);ref.text(3,image.alt);ref.integer(4,ordinal++);ref.run();
            }
            Statement stale(db_,"DELETE FROM article_images WHERE entry_id=? AND ordinal=-1");stale.integer(1,id);stale.run();
        }
        if (!parsed.title.empty()) {
            Statement title(db_, "UPDATE feeds SET title=? WHERE id=? AND (title='' OR title=url)");
            title.text(1, parsed.title); title.integer(2, feed_id); title.run();
        }
        exec(db_, "COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

void Store::sync_result(std::int64_t feed_id, const std::string& error, const std::string& etag,
                        const std::string& modified, std::int64_t now) {
    Statement update(db_, error.empty()
        ? "UPDATE feeds SET error='',etag=?,modified=?,synced_at=? WHERE id=?"
        : "UPDATE feeds SET error=? WHERE id=?");
    if (error.empty()) { update.text(1, etag); update.text(2, modified); update.integer(3, now); update.integer(4, feed_id); }
    else { update.text(1, error); update.integer(2, feed_id); }
    update.run();
}

std::vector<Entry> Store::entries(View view, std::int64_t feed_id, const std::string& search, std::int64_t today_start,
                                const std::optional<std::string>& group) const {
    std::string sql = std::string("SELECT ")+metadata_columns+" FROM entries WHERE 1=1";
    if (view == View::Unread) sql += " AND read=0";
    if (view == View::Starred) sql += " AND starred=1";
    if (view == View::Today) sql += " AND published>=?";
    if (feed_id) sql += " AND feed_id=?";
    if (group) sql += group->empty()
        ? " AND feed_id IN (SELECT id FROM feeds WHERE active=1 AND group_name=?)"
        : " AND feed_id IN (SELECT id FROM feeds WHERE active=1 AND (group_name=? OR instr(group_name,?)=1))";
    if (!search.empty()) sql += " AND title LIKE ? ESCAPE '\\'";
    sql += " ORDER BY published DESC,id DESC";
    Statement query(db_, sql.c_str());
    int bind = 1;
    if (view == View::Today) query.integer(bind++, today_start);
    if (feed_id) query.integer(bind++, feed_id);
    if (group) { query.text(bind++, *group); if (!group->empty()) query.text(bind++, *group+"/"); }
    if (!search.empty()) {
        std::string pattern;
        for (char c : search) { if (c == '%' || c == '_' || c == '\\') pattern += '\\'; pattern += c; }
        query.text(bind, "%" + pattern + "%");
    }
    std::vector<Entry> result;
    int step;
    while ((step = sqlite3_step(query.get())) == SQLITE_ROW) result.push_back(read_entry(query.get()));
    check(db_, step);
    return result;
}

std::optional<Entry> Store::entry(std::int64_t id, bool with_content) const {
    Statement query(db_, (std::string("SELECT ") + (with_content?entry_columns:metadata_columns) + " FROM entries WHERE id=?").c_str());
    query.integer(1, id);
    const int step = sqlite3_step(query.get());
    if (step == SQLITE_DONE) return std::nullopt;
    check(db_, step);
    auto entry=read_entry(query.get());
    if(!with_content)return entry;
    Statement images(db_,"SELECT url,alt,error,width,height FROM article_images WHERE entry_id=? ORDER BY ordinal");images.integer(1,id);
    int result;
    while((result=sqlite3_step(images.get()))==SQLITE_ROW){
        ArticleImage image;image.url=column_text(images.get(),0);image.alt=column_text(images.get(),1);image.error=column_text(images.get(),2);
        image.width=sqlite3_column_int(images.get(),3);image.height=sqlite3_column_int(images.get(),4);entry.images.push_back(std::move(image));
    }
    check(db_,result);return entry;
}

std::optional<ArticleImage> Store::image(std::int64_t id,const std::string& url) const {
    Statement q(db_,"SELECT alt,error,width,height,pixels FROM article_images WHERE entry_id=? AND url=?");q.integer(1,id);q.text(2,url);
    const int result=sqlite3_step(q.get());check(db_,result);if(result==SQLITE_DONE)return std::nullopt;
    ArticleImage image;image.url=url;image.alt=column_text(q.get(),0);image.error=column_text(q.get(),1);
    image.width=sqlite3_column_int(q.get(),2);image.height=sqlite3_column_int(q.get(),3);
    const auto size=sqlite3_column_bytes(q.get(),4);
    if(size<0||size>1072*1448)throw std::runtime_error("图片缓存尺寸异常");
    if(size){const auto* pixels=static_cast<const std::uint8_t*>(sqlite3_column_blob(q.get(),4));image.pixels.assign(pixels,pixels+size);}
    return image;
}
void Store::save_image(std::int64_t id,const ArticleImage& image){
    if(!image.error.empty()){
        Statement q(db_,"UPDATE article_images SET error=? WHERE entry_id=? AND url=? AND width=0");
        q.text(1,image.error);q.integer(2,id);q.text(3,image.url);q.run();return;
    }
    if(image.width<=0||image.width>1072||image.height<=0||image.height>1448||
       image.pixels.size()!=static_cast<std::size_t>(image.width)*image.height)
        throw std::runtime_error("无效图片缓存尺寸");
    Statement q(db_,"UPDATE article_images SET width=?,height=?,pixels=?,error='' WHERE entry_id=? AND url=?");
    q.integer(1,image.width);q.integer(2,image.height);q.blob(3,image.pixels);q.integer(4,id);q.text(5,image.url);q.run();
}
std::vector<std::pair<std::int64_t,ArticleImage>> Store::pending_images() const {
    Statement q(db_,"SELECT i.entry_id,i.url,i.alt FROM article_images i JOIN entries e ON e.id=i.entry_id "
                    "JOIN feeds f ON f.id=e.feed_id WHERE i.width=0 AND f.active=1 ORDER BY (i.error<>''),e.published DESC,i.ordinal LIMIT 100");
    std::vector<std::pair<std::int64_t,ArticleImage>> result;int code;
    while((code=sqlite3_step(q.get()))==SQLITE_ROW){
        ArticleImage image;image.url=column_text(q.get(),1);image.alt=column_text(q.get(),2);
        result.emplace_back(sqlite3_column_int64(q.get(),0),std::move(image));
    }
    check(db_,code);return result;
}

void Store::set_read(std::int64_t id, bool value) { Statement q(db_, "UPDATE entries SET read=? WHERE id=?"); q.boolean(1,value); q.integer(2,id); q.run(); }
void Store::set_starred(std::int64_t id, bool value) { Statement q(db_, "UPDATE entries SET starred=? WHERE id=?"); q.boolean(1,value); q.integer(2,id); q.run(); }
void Store::set_position(std::int64_t id, std::int64_t offset) { Statement q(db_, "UPDATE entries SET position=?,in_progress=1 WHERE id=?"); q.integer(1,std::max<std::int64_t>(0,offset)); q.integer(2,id); q.run(); }
void Store::finish_reading(std::int64_t id) { Statement q(db_, "UPDATE entries SET in_progress=0,position=0,read=1 WHERE id=?"); q.integer(1,id); q.run(); }

void Store::mark_feed_read(std::int64_t feed_id) {
    Statement q(db_, feed_id ? "UPDATE entries SET read=1 WHERE feed_id=?" : "UPDATE entries SET read=1");
    if (feed_id) q.integer(1,feed_id);
    q.run();
}

void Store::remove_feed(std::int64_t id) {
    exec(db_, "BEGIN IMMEDIATE");
    try {
        Statement inactive(db_, "UPDATE feeds SET active=0 WHERE id=?"); inactive.integer(1,id); inactive.run();
        Statement remove(db_, "DELETE FROM entries WHERE feed_id=? AND starred=0"); remove.integer(1,id); remove.run();
        exec(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

std::size_t Store::clean_read(std::int64_t before) {
    Statement q(db_, "DELETE FROM entries WHERE read=1 AND starred=0 AND in_progress=0 AND published<?"); q.integer(1,before); q.run();
    return static_cast<std::size_t>(sqlite3_changes(db_));
}

void Store::set_setting(const std::string& key, const std::string& value) {
    Statement q(db_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.text(1,key); q.text(2,value); q.run();
}

std::string Store::setting(const std::string& key, const std::string& fallback) const {
    Statement q(db_, "SELECT value FROM settings WHERE key=?"); q.text(1,key);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return fallback;
    check(db_,step);
    return column_text(q.get(),0);
}
}
