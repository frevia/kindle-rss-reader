#include "store.hpp"
#include "translation.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <filesystem>
#include <set>
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

std::string entry_filter_sql(View view, std::int64_t feed_id, const std::string& search,
                             const std::optional<std::string>& group) {
    std::string sql = " WHERE 1=1";
    if (view == View::Unread) sql += " AND read=0";
    if (view == View::Starred) sql += " AND starred=1";
    if (view == View::Today) sql += " AND published>=?";
    if (feed_id) sql += " AND feed_id=?";
    if (group) sql += group->empty()
        ? " AND feed_id IN (SELECT id FROM feeds WHERE active=1 AND group_name=?)"
        : " AND feed_id IN (SELECT id FROM feeds WHERE active=1 AND (group_name=? OR instr(group_name,?)=1))";
    if (!search.empty()) sql += " AND title LIKE ? ESCAPE '\\'";
    return sql;
}

void bind_entry_filters(Statement& query, View view, std::int64_t feed_id, const std::string& search,
                        std::int64_t today_start, const std::optional<std::string>& group) {
    int bind = 1;
    if (view == View::Today) query.integer(bind++, today_start);
    if (feed_id) query.integer(bind++, feed_id);
    if (group) { query.text(bind++, *group); if (!group->empty()) query.text(bind++, *group + "/"); }
    if (!search.empty()) {
        std::string pattern;
        for (char c : search) { if (c == '%' || c == '_' || c == '\\') pattern += '\\'; pattern += c; }
        query.text(bind, "%" + pattern + "%");
    }
}

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

void validate_group(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path.front() == '/' || path.back() == '/' || path.find("//") != std::string::npos)
        throw std::runtime_error("无效分组路径");
    for (unsigned char c : path) if (c < 32 || c == 127) throw std::runtime_error("分组路径不能包含控制字符");
}
bool group_is_or_below(const std::string& value, const std::string& path) {
    return value == path || (value.size() > path.size() && value.compare(0,path.size(),path) == 0 && value[path.size()] == '/');
}
void validate_feed_url(const std::string& url) {
    if (url.empty() || url.size() > 2048) throw std::runtime_error("订阅地址长度无效");
    for (unsigned char c : url) if (c < 32 || c == 127) throw std::runtime_error("订阅地址不能包含控制字符");
    CURLU* parsed = curl_url();
    if (!parsed) throw std::runtime_error("无法验证订阅地址");
    char *scheme = nullptr, *host = nullptr;
    const bool valid = curl_url_set(parsed,CURLUPART_URL,url.c_str(),0) == CURLUE_OK &&
        curl_url_get(parsed,CURLUPART_SCHEME,&scheme,0) == CURLUE_OK && curl_url_get(parsed,CURLUPART_HOST,&host,0) == CURLUE_OK &&
        host && *host && scheme && (std::string(scheme) == "http" || std::string(scheme) == "https");
    curl_free(scheme); curl_free(host); curl_url_cleanup(parsed);
    if (!valid) throw std::runtime_error("订阅地址必须是有效的 HTTP(S) URL");
}
void validate_selection(const SelectionJob& job) {
    const auto mode=static_cast<int>(job.mode);
    if(job.entry_id<=0||mode<0||mode>2||job.revision.empty()||job.revision.size()>1024||job.revision.find('\0')!=std::string::npos||
       job.range.start.empty()||job.range.start.size()>4096||job.range.start.find('\0')!=std::string::npos||
       job.range.end.empty()||job.range.end.size()>4096||job.range.end.find('\0')!=std::string::npos||job.range.start==job.range.end||
       job.range.text.empty()||job.range.text.size()>6000||job.range.text.find('\0')!=std::string::npos)
        throw std::runtime_error("无效选区");
    try { translation_units(job.revision);translation_units(job.range.start); translation_units(job.range.end);if(!translation_units(job.range.text))throw std::runtime_error("空选区"); }
    catch (...) { throw std::runtime_error("选区包含无效 UTF-8"); }
}
void validate_selection_translation(const std::string& translated) {
    if(translated.empty()||translated.size()>64*1024||translated.find('\0')!=std::string::npos)
        throw std::runtime_error("无效选区译文");
    try { if(!translation_units(translated))throw std::runtime_error("空译文"); }
    catch (...) { throw std::runtime_error("选区译文包含无效 UTF-8"); }
}
bool valid_http_url(const std::string& url) {
    if (url.empty() || url.size() > 2048 || std::any_of(url.begin(),url.end(),[](unsigned char c){return c<=32||c==127;})) return false;
    CURLU* parsed = curl_url();
    if (!parsed) return false;
    char *scheme = nullptr, *host = nullptr;
    const bool valid = curl_url_set(parsed,CURLUPART_URL,url.c_str(),0) == CURLUE_OK &&
        curl_url_get(parsed,CURLUPART_SCHEME,&scheme,0) == CURLUE_OK && curl_url_get(parsed,CURLUPART_HOST,&host,0) == CURLUE_OK &&
        host && *host && scheme && (std::string(scheme) == "http" || std::string(scheme) == "https");
    curl_free(scheme); curl_free(host); curl_url_cleanup(parsed);
    return valid;
}
void add_group_rows(sqlite3* db, const std::string& path) {
    Statement insert(db,"INSERT OR IGNORE INTO feed_groups(path) VALUES(?)");
    for (std::size_t end=0; end<path.size();) {
        end=path.find('/',end); const auto part=end==std::string::npos?path.size():end;
        check(db,sqlite3_reset(insert.get())); check(db,sqlite3_clear_bindings(insert.get()));
        insert.text(1,path.substr(0,part)); insert.run();
        if(end==std::string::npos) break; ++end;
    }
}
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
        exec(db_, "CREATE TABLE IF NOT EXISTS feed_groups (path TEXT PRIMARY KEY)");
        exec(db_, "CREATE TABLE IF NOT EXISTS article_images (entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
                  "url TEXT NOT NULL,alt TEXT NOT NULL DEFAULT '',ordinal INTEGER NOT NULL,error TEXT NOT NULL DEFAULT '',"
                  "width INTEGER NOT NULL DEFAULT 0,height INTEGER NOT NULL DEFAULT 0,pixels BLOB NOT NULL DEFAULT X'',"
                  "PRIMARY KEY(entry_id,url))");
        exec(db_, "BEGIN IMMEDIATE");
        try {
            exec(db_, "CREATE TABLE IF NOT EXISTS article_fulltext (entry_id INTEGER PRIMARY KEY REFERENCES entries(id) ON DELETE CASCADE,"
                      "html TEXT NOT NULL DEFAULT '',text TEXT NOT NULL DEFAULT '',url TEXT NOT NULL DEFAULT '',"
                      "state TEXT NOT NULL DEFAULT 'pending',error TEXT NOT NULL DEFAULT '',attempted_at INTEGER NOT NULL DEFAULT 0,"
                      "position INTEGER NOT NULL DEFAULT 0,automatic INTEGER NOT NULL DEFAULT 0)");
            exec(db_, "CREATE TABLE IF NOT EXISTS article_fulltext_images (entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
                      "url TEXT NOT NULL,alt TEXT NOT NULL DEFAULT '',ordinal INTEGER NOT NULL,PRIMARY KEY(entry_id,ordinal))");
            exec(db_, "CREATE TABLE IF NOT EXISTS article_translations (entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
                      "web INTEGER NOT NULL,revision TEXT NOT NULL,state TEXT NOT NULL DEFAULT 'pending',error TEXT NOT NULL DEFAULT '',"
                      "total INTEGER NOT NULL,PRIMARY KEY(entry_id,web,revision))");
            exec(db_, "CREATE TABLE IF NOT EXISTS article_translation_blocks (entry_id INTEGER NOT NULL,web INTEGER NOT NULL,revision TEXT NOT NULL,"
                      "ordinal INTEGER NOT NULL,html TEXT NOT NULL,PRIMARY KEY(entry_id,web,revision,ordinal),"
                      "FOREIGN KEY(entry_id,web,revision) REFERENCES article_translations(entry_id,web,revision) ON DELETE CASCADE)");
            exec(db_, "CREATE TABLE IF NOT EXISTS article_selections (entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,"
                      "web INTEGER NOT NULL,mode INTEGER NOT NULL,revision TEXT NOT NULL,start_anchor TEXT NOT NULL,end_anchor TEXT NOT NULL,"
                      "text TEXT NOT NULL,highlighted INTEGER NOT NULL DEFAULT 0,translation TEXT NOT NULL DEFAULT '',"
                      "PRIMARY KEY(entry_id,web,mode,revision,start_anchor,end_anchor))");
            exec(db_, "COMMIT");
        } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
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
    validate_feed_url(feed.url);
    Statement insert(db_, "INSERT INTO feeds(url,title,group_name) VALUES(?,?,?) ON CONFLICT(url) DO UPDATE SET active=1");
    insert.text(1, feed.url); insert.text(2, feed.title); insert.text(3, feed.group); insert.run();
    Statement select(db_, "SELECT id FROM feeds WHERE url=?");
    select.text(1, feed.url);
    check(db_, sqlite3_step(select.get()));
    return sqlite3_column_int64(select.get(), 0);
}

void Store::edit_feed(std::int64_t id, const std::string& title, const std::string& group) {
    if (title.size()>512 || group.size()>1024 ||
        title.find_first_of("\r\n\t")!=std::string::npos || group.find_first_of("\r\n\t")!=std::string::npos)
        throw std::runtime_error("名称和分组必须为单行且长度适中");
    Statement q(db_, "UPDATE feeds SET title=?,group_name=?,etag=CASE WHEN ?='' THEN '' ELSE etag END,modified=CASE WHEN ?='' THEN '' ELSE modified END WHERE id=? AND active=1");
    q.text(1,title); q.text(2,group); q.text(3,title); q.text(4,title); q.integer(5,id); q.run();
    if (!sqlite3_changes(db_)) throw std::runtime_error("订阅已不存在");
}

void Store::update_feed(const Feed& feed) {
    if (feed.title.size()>512 || feed.group.size()>1024 ||
        feed.title.find_first_of("\r\n\t")!=std::string::npos || feed.group.find_first_of("\r\n\t")!=std::string::npos)
        throw std::runtime_error("名称和分组必须为单行且长度适中");
    if (!feed.group.empty()) validate_group(feed.group);
    validate_feed_url(feed.url);
    exec(db_, "BEGIN IMMEDIATE");
    try {
        {
            Statement conflict(db_, "SELECT active,EXISTS(SELECT 1 FROM entries WHERE feed_id=feeds.id) FROM feeds WHERE url=? AND id<>?");
            conflict.text(1,feed.url); conflict.integer(2,feed.id);
            const int step=sqlite3_step(conflict.get()); check(db_,step);
            if (step==SQLITE_ROW) {
                if (sqlite3_column_int(conflict.get(),0)) throw std::runtime_error("此地址已被其他订阅使用");
                if (sqlite3_column_int(conflict.get(),1)) throw std::runtime_error("此地址保留着已取消订阅的收藏，请通过添加订阅恢复原订阅");
                // Only an empty, inactive record can release its address; never merge reading history.
                Statement release(db_, "DELETE FROM feeds WHERE url=? AND active=0 AND NOT EXISTS(SELECT 1 FROM entries WHERE feed_id=feeds.id)");
                release.text(1,feed.url); release.run();
            }
        }
        Statement q(db_,"UPDATE feeds SET url=?,title=?,group_name=?,etag=CASE WHEN ?='' OR url<>? THEN '' ELSE etag END,modified=CASE WHEN ?='' OR url<>? THEN '' ELSE modified END,synced_at=CASE WHEN url<>? THEN 0 ELSE synced_at END WHERE id=? AND active=1");
        q.text(1,feed.url); q.text(2,feed.title); q.text(3,feed.group); q.text(4,feed.title); q.text(5,feed.url); q.text(6,feed.title); q.text(7,feed.url); q.text(8,feed.url); q.integer(9,feed.id); q.run();
        if (!sqlite3_changes(db_)) throw std::runtime_error("订阅已不存在");
        exec(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
}

std::vector<Feed> Store::feeds() const {
    Statement query(db_, (std::string("SELECT ") + feed_columns + " FROM feeds WHERE active=1 ORDER BY id").c_str());
    std::vector<Feed> result;
    while (sqlite3_step(query.get()) == SQLITE_ROW) result.push_back(read_feed(query.get()));
    check(db_, sqlite3_errcode(db_));
    return result;
}

std::vector<std::string> Store::groups() const {
    std::set<std::string> result{ "" };
    Statement saved(db_,"SELECT path FROM feed_groups"); int step;
    while((step=sqlite3_step(saved.get()))==SQLITE_ROW) result.insert(column_text(saved.get(),0));
    check(db_,step);
    Statement active(db_,"SELECT group_name FROM feeds WHERE active=1 AND group_name<>''");
    while((step=sqlite3_step(active.get()))==SQLITE_ROW) {
        const auto path=column_text(active.get(),0);
        for(std::size_t end=0;end<path.size();) {
            end=path.find('/',end); result.insert(path.substr(0,end==std::string::npos?path.size():end));
            if(end==std::string::npos) break; ++end;
        }
    }
    check(db_,step); return {result.begin(),result.end()};
}

void Store::add_group(const std::string& path) {
    validate_group(path); exec(db_,"BEGIN IMMEDIATE");
    try {
        const auto existing=groups();
        if(std::find(existing.begin(),existing.end(),path)!=existing.end()) throw std::runtime_error("分组已存在");
        add_group_rows(db_,path); exec(db_,"COMMIT");
    }
    catch(...) { sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
}

void Store::rename_group(const std::string& old_path,const std::string& new_path) {
    validate_group(old_path); validate_group(new_path);
    if(old_path==new_path) return;
    if(group_is_or_below(new_path,old_path)) throw std::runtime_error("不能将分组移入自身子树");
    exec(db_,"BEGIN IMMEDIATE");
    try {
        const auto current=groups();std::set<std::string> existing(current.begin(),current.end());existing.erase("");
        if(!existing.count(old_path)) throw std::runtime_error("分组不存在");
        std::vector<std::pair<std::string,std::string>> paths;
        for(const auto& p:existing) if(group_is_or_below(p,old_path)) {
            auto renamed=new_path+p.substr(old_path.size());
            validate_group(renamed);
            if(existing.count(renamed)&&!group_is_or_below(renamed,old_path)) throw std::runtime_error("目标分组已存在");
            paths.emplace_back(p,std::move(renamed));
        }
        for(const auto& path:existing) add_group_rows(db_,path);
        for(const auto& p:paths) { Statement q(db_,"DELETE FROM feed_groups WHERE path=?"); q.text(1,p.first); q.run(); }
        for(const auto& p:paths) { Statement q(db_,"INSERT INTO feed_groups(path) VALUES(?)"); q.text(1,p.second); q.run(); }
        Statement update(db_,"UPDATE feeds SET group_name=? || substr(group_name,length(?)+1) WHERE active=1 AND (group_name=? OR (length(group_name)>length(?) AND substr(group_name,1,length(?))=? AND substr(group_name,length(?)+1,1)='/'))");
        update.text(1,new_path); update.text(2,old_path); update.text(3,old_path); update.text(4,old_path); update.text(5,old_path); update.text(6,old_path); update.text(7,old_path); update.run();
        add_group_rows(db_,new_path); exec(db_,"COMMIT");
    } catch(...) { sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
}

void Store::remove_group(const std::string& path) {
    validate_group(path); exec(db_,"BEGIN IMMEDIATE");
    try {
        const auto existing=groups();
        if(std::find(existing.begin(),existing.end(),path)==existing.end()) throw std::runtime_error("分组不存在");
        for(const auto& group:existing)if(!group.empty()&&!group_is_or_below(group,path))add_group_rows(db_,group);
        Statement update(db_,"UPDATE feeds SET group_name='' WHERE active=1 AND (group_name=? OR (length(group_name)>length(?) AND substr(group_name,1,length(?))=? AND substr(group_name,length(?)+1,1)='/'))");
        update.text(1,path); update.text(2,path); update.text(3,path); update.text(4,path); update.text(5,path); update.run();
        Statement remove(db_,"DELETE FROM feed_groups WHERE path=? OR (length(path)>length(?) AND substr(path,1,length(?))=? AND substr(path,length(?)+1,1)='/')");
        remove.text(1,path); remove.text(2,path); remove.text(3,path); remove.text(4,path); remove.text(5,path); remove.run();
        exec(db_,"COMMIT");
    } catch(...) { sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
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
            Statement prior(db_,"SELECT 1 FROM entries WHERE feed_id=? AND guid=?");
            prior.integer(1,feed_id);prior.text(2,entry.guid);
            const int prior_step=sqlite3_step(prior.get());check(db_,prior_step);
            const bool is_new=prior_step==SQLITE_DONE;
            check(db_, sqlite3_reset(save.get()));
            check(db_, sqlite3_clear_bindings(save.get()));
            save.integer(1, feed_id); save.text(2, entry.guid); save.text(3, entry.title); save.text(4, entry.url);
            save.text(5, entry.author); save.text(6, entry.html); save.text(7, entry.text);
            save.integer(8, entry.published); save.integer(9, entry.published); save.integer(10, entry.published);
            save.run();
            Statement lookup(db_,"SELECT id FROM entries WHERE feed_id=? AND guid=?");
            lookup.integer(1,feed_id);lookup.text(2,entry.guid);check(db_,sqlite3_step(lookup.get()));
            const auto id=sqlite3_column_int64(lookup.get(),0);
            // Leave retained image rows untouched: rewriting their pixel blobs can block foreground reading on flash.
            const auto image_count=std::min<std::size_t>(20,entry.images.size());
            std::string stale_sql="UPDATE article_images SET ordinal=-1 WHERE entry_id=? AND ordinal>=0";
            if(image_count){stale_sql+=" AND url NOT IN (";for(std::size_t i=0;i<image_count;++i)stale_sql+=i?",?":"?";stale_sql+=")";}
            Statement old(db_,stale_sql.c_str());old.integer(1,id);
            for(std::size_t i=0;i<image_count;++i)old.text(static_cast<int>(i)+2,entry.images[i].url);
            old.run();
            int ordinal=0;
            for(const auto& image:entry.images){
                if(ordinal==20)break;
                Statement ref(db_,"INSERT INTO article_images(entry_id,url,alt,ordinal) VALUES(?,?,?,?) "
                                  "ON CONFLICT(entry_id,url) DO UPDATE SET alt=excluded.alt,ordinal=excluded.ordinal "
                                  "WHERE article_images.alt<>excluded.alt OR article_images.ordinal<>excluded.ordinal");
                ref.integer(1,id);ref.text(2,image.url);ref.text(3,image.alt);ref.integer(4,ordinal++);ref.run();
            }
            Statement stale(db_,"DELETE FROM article_images WHERE entry_id=? AND ordinal=-1 AND NOT EXISTS(SELECT 1 FROM article_fulltext_images f WHERE f.entry_id=article_images.entry_id AND f.url=article_images.url)");stale.integer(1,id);stale.run();
            if (is_new && setting("feed_fulltext:" + std::to_string(feed_id)) == "1") {
                if (valid_http_url(entry.url)) {
                    Statement queue(db_, "INSERT INTO article_fulltext(entry_id,state,automatic) VALUES(?,'pending',1)");
                    queue.integer(1,id); queue.run();
                }
            }
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
    const std::string sql = std::string("SELECT ") + metadata_columns + " FROM entries" +
        entry_filter_sql(view, feed_id, search, group) + " ORDER BY published DESC,id DESC";
    Statement query(db_, sql.c_str());
    bind_entry_filters(query, view, feed_id, search, today_start, group);
    std::vector<Entry> result;
    int step;
    while ((step = sqlite3_step(query.get())) == SQLITE_ROW) result.push_back(read_entry(query.get()));
    check(db_, step);
    return result;
}

std::int64_t Store::entry_count(View view, std::int64_t feed_id, const std::string& search, std::int64_t today_start,
                                const std::optional<std::string>& group) const {
    const std::string sql = "SELECT count(*) FROM entries" + entry_filter_sql(view, feed_id, search, group);
    Statement query(db_, sql.c_str());
    bind_entry_filters(query, view, feed_id, search, today_start, group);
    const int step = sqlite3_step(query.get());
    if (step != SQLITE_ROW) {
        check(db_, step);
        throw std::runtime_error("entry count query returned no result");
    }
    return sqlite3_column_int64(query.get(), 0);
}

std::optional<Entry> Store::entry(std::int64_t id, bool with_content) const {
    Statement query(db_, (std::string("SELECT ") + (with_content?entry_columns:metadata_columns) + " FROM entries WHERE id=?").c_str());
    query.integer(1, id);
    const int step = sqlite3_step(query.get());
    if (step == SQLITE_DONE) return std::nullopt;
    check(db_, step);
    auto entry=read_entry(query.get());
    if(!with_content)return entry;
    Statement images(db_,"SELECT url,alt,error,width,height FROM article_images WHERE entry_id=? AND ordinal>=0 ORDER BY ordinal");images.integer(1,id);
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
FulltextStatus Store::fulltext_status(std::int64_t id) const {
    Statement q(db_,"SELECT state,error FROM article_fulltext WHERE entry_id=?");q.integer(1,id);
    const int step=sqlite3_step(q.get());check(db_,step);if(step==SQLITE_DONE)return {};
    return {column_text(q.get(),0),column_text(q.get(),1)};
}

bool Store::queue_fulltext(std::int64_t id) {
    exec(db_,"BEGIN IMMEDIATE");
    try {
    Statement article(db_,"SELECT e.url FROM entries e JOIN feeds f ON f.id=e.feed_id WHERE e.id=? AND f.active=1");article.integer(1,id);
    int step=sqlite3_step(article.get());check(db_,step);
    if(step==SQLITE_DONE||!valid_http_url(column_text(article.get(),0))){exec(db_,"COMMIT");return false;}
    Statement status(db_,"SELECT state FROM article_fulltext WHERE entry_id=?");status.integer(1,id);
    step=sqlite3_step(status.get());check(db_,step);
    if(step==SQLITE_ROW){
        const auto state=column_text(status.get(),0);
        if(state=="pending"){
            Statement manual(db_,"UPDATE article_fulltext SET automatic=0 WHERE entry_id=? AND state='pending'");manual.integer(1,id);manual.run();exec(db_,"COMMIT");return false;
        }
        if(state!="failed"){exec(db_,"COMMIT");return false;}
        Statement retry(db_,"UPDATE article_fulltext SET state='pending',error='',automatic=0 WHERE entry_id=? AND state='failed'");retry.integer(1,id);retry.run();
        const bool queued=sqlite3_changes(db_)!=0;exec(db_,"COMMIT");return queued;
    }
    Statement insert(db_,"INSERT INTO article_fulltext(entry_id,state,automatic) VALUES(?,'pending',0)");insert.integer(1,id);insert.run();
    exec(db_,"COMMIT");return true;
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

std::vector<Entry> Store::pending_fulltext(bool retry_failed,std::size_t limit) const {
    const char* state=retry_failed?"failed":"pending";
    Statement q(db_,std::string("SELECT e.id,e.feed_id,e.guid,e.title,e.url,e.author,'' AS html,'' AS text,e.published,e.read,e.starred,e.position,e.in_progress FROM entries e JOIN article_fulltext a ON a.entry_id=e.id JOIN feeds f ON f.id=e.feed_id "
        "WHERE f.active=1 AND a.state=? AND (a.automatic=0 OR (SELECT value FROM settings WHERE key='feed_fulltext:' || f.id)='1') "
        "ORDER BY e.published DESC,e.id DESC LIMIT ?").c_str());
    q.text(1,state);q.integer(2,static_cast<std::int64_t>(std::min<std::size_t>(limit,static_cast<std::size_t>(INT64_MAX))));
    std::vector<Entry> result;int step;
    while((step=sqlite3_step(q.get()))==SQLITE_ROW)result.push_back(read_entry(q.get()));
    check(db_,step);return result;
}

void Store::save_fulltext(std::int64_t id,const Entry& content) {
    if(content.html.empty()||content.html.size()>512*1024||content.text.empty()||content.text.size()>512*1024||!valid_http_url(content.url)||content.images.size()>20)
        throw std::runtime_error("无效全文内容");
    for(const auto& image:content.images)if(!valid_http_url(image.url))throw std::runtime_error("无效全文图片地址");
    exec(db_,"BEGIN IMMEDIATE");
    try {
        Statement active(db_,"SELECT 1 FROM entries e JOIN feeds f ON f.id=e.feed_id WHERE e.id=? AND f.active=1");active.integer(1,id);
        int step=sqlite3_step(active.get());check(db_,step);
        if(step==SQLITE_DONE){exec(db_,"COMMIT");return;}
        Statement update(db_,"UPDATE article_fulltext SET html=?,text=?,url=?,state='ready',error='',attempted_at=CAST(strftime('%s','now') AS INTEGER) WHERE entry_id=? AND state='pending'");
        update.text(1,content.html);update.text(2,content.text);update.text(3,content.url);update.integer(4,id);update.run();
        if(!sqlite3_changes(db_)){exec(db_,"COMMIT");return;}
        Statement clear(db_,"DELETE FROM article_fulltext_images WHERE entry_id=?");clear.integer(1,id);clear.run();
        int ordinal=0;
        for(const auto& image:content.images){
            Statement shared(db_,"INSERT OR IGNORE INTO article_images(entry_id,url,alt,ordinal) VALUES(?,?,?,-1)");shared.integer(1,id);shared.text(2,image.url);shared.text(3,image.alt);shared.run();
            Statement ref(db_,"INSERT INTO article_fulltext_images(entry_id,url,alt,ordinal) VALUES(?,?,?,?)");ref.integer(1,id);ref.text(2,image.url);ref.text(3,image.alt);ref.integer(4,ordinal++);ref.run();
        }
        exec(db_,"COMMIT");
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void Store::fail_fulltext(std::int64_t id,const std::string& error) {
    Statement q(db_,"UPDATE article_fulltext SET state='failed',error=?,attempted_at=CAST(strftime('%s','now') AS INTEGER) WHERE entry_id=? AND state IN ('pending','failed')");
    q.text(1,error);q.integer(2,id);q.run();
}

std::optional<Entry> Store::fulltext_entry(std::int64_t id) const {
    Statement q(db_,"SELECT e.id,e.feed_id,e.guid,e.title,e.url,e.author,'' AS html,'' AS text,e.published,e.read,e.starred,e.position,e.in_progress,a.html,a.text,a.url,a.position FROM entries e JOIN article_fulltext a ON a.entry_id=e.id WHERE e.id=? AND a.state='ready'");
    q.integer(1,id);int step=sqlite3_step(q.get());check(db_,step);if(step==SQLITE_DONE)return std::nullopt;
    auto entry=read_entry(q.get());entry.html=column_text(q.get(),13);entry.text=column_text(q.get(),14);entry.url=column_text(q.get(),15);entry.position=sqlite3_column_int64(q.get(),16);entry.web_content=true;
    Statement images(db_,"SELECT f.url,f.alt,i.error,i.width,i.height FROM article_fulltext_images f LEFT JOIN article_images i ON i.entry_id=f.entry_id AND i.url=f.url WHERE f.entry_id=? ORDER BY f.ordinal");images.integer(1,id);
    while((step=sqlite3_step(images.get()))==SQLITE_ROW){ArticleImage image;image.url=column_text(images.get(),0);image.alt=column_text(images.get(),1);image.error=column_text(images.get(),2);image.width=sqlite3_column_int(images.get(),3);image.height=sqlite3_column_int(images.get(),4);entry.images.push_back(std::move(image));}
    check(db_,step);return entry;
}

void Store::begin_translation(std::int64_t id,bool web,const std::string& revision,std::size_t total) {
    const auto source=web?fulltext_entry(id):entry(id);
    if(!source||translation_revision(*source)!=revision)throw std::runtime_error("文章已更新或删除，请重新打开后翻译");
    if(total==0||total>2048||revision.size()>128)throw std::runtime_error("翻译段落数量无效");
    Statement q(db_,"INSERT INTO article_translations(entry_id,web,revision,total) VALUES(?,?,?,?) "
                    "ON CONFLICT(entry_id,web,revision) DO UPDATE SET state='pending',error=''");
    q.integer(1,id);q.boolean(2,web);q.text(3,revision);q.integer(4,static_cast<std::int64_t>(total));q.run();
}
TranslationStatus Store::translation_status(std::int64_t id,bool web,const std::string& revision) const {
    Statement q(db_,"SELECT state,error,total,(SELECT count(*) FROM article_translation_blocks b WHERE b.entry_id=t.entry_id AND b.web=t.web AND b.revision=t.revision) "
                    "FROM article_translations t WHERE entry_id=? AND web=? AND revision=?");
    q.integer(1,id);q.boolean(2,web);q.text(3,revision);
    const auto result=sqlite3_step(q.get());check(db_,result);
    if(result==SQLITE_ROW)return {column_text(q.get(),0),column_text(q.get(),1),static_cast<std::size_t>(sqlite3_column_int64(q.get(),3)),static_cast<std::size_t>(sqlite3_column_int64(q.get(),2))};
    Statement stale(db_,"SELECT 1 FROM article_translations WHERE entry_id=? AND web=? LIMIT 1");stale.integer(1,id);stale.boolean(2,web);
    const auto previous=sqlite3_step(stale.get());check(db_,previous);
    return {previous==SQLITE_ROW?"stale":"","",0,0};
}
std::vector<std::string> Store::translation_blocks(std::int64_t id,bool web,const std::string& revision) const {
    const auto status=translation_status(id,web,revision);
    if(status.total>2048)throw std::runtime_error("翻译缓存段落数量无效");
    std::vector<std::string> blocks(status.total);
    Statement q(db_,"SELECT ordinal,html FROM article_translation_blocks WHERE entry_id=? AND web=? AND revision=? ORDER BY ordinal");
    q.integer(1,id);q.boolean(2,web);q.text(3,revision);int step;std::size_t bytes=0;
    while((step=sqlite3_step(q.get()))==SQLITE_ROW) {
        const auto ordinal=sqlite3_column_int64(q.get(),0);
        if(ordinal<0||static_cast<std::size_t>(ordinal)>=blocks.size())throw std::runtime_error("翻译缓存段落顺序无效");
        blocks[static_cast<std::size_t>(ordinal)]=column_text(q.get(),1);bytes+=blocks[static_cast<std::size_t>(ordinal)].size();
        if(bytes>1024*1024)throw std::runtime_error("翻译缓存超过大小限制");
    }
    check(db_,step);return blocks;
}
void Store::save_translation_block(std::int64_t id,bool web,const std::string& revision,std::size_t block,const std::string& html) {
    if(html.size()>512*1024)throw std::runtime_error("翻译段落超过大小限制");
    exec(db_,"BEGIN IMMEDIATE");
    try {
        const auto source=web?fulltext_entry(id):entry(id);
        if(!source||translation_revision(*source)!=revision)throw std::runtime_error("文章已更新或删除，请重新打开后翻译");
        Statement q(db_,"INSERT INTO article_translation_blocks(entry_id,web,revision,ordinal,html) "
                        "SELECT entry_id,web,revision,?,? FROM article_translations WHERE entry_id=? AND web=? AND revision=? AND total>? "
                        "AND coalesce((SELECT sum(length(CAST(html AS BLOB))) FROM article_translation_blocks WHERE entry_id=? AND web=? AND revision=?),0)+?<=1048576 "
                        "ON CONFLICT(entry_id,web,revision,ordinal) DO UPDATE SET html=excluded.html");
        q.integer(1,static_cast<std::int64_t>(block));q.text(2,html);q.integer(3,id);q.boolean(4,web);q.text(5,revision);q.integer(6,static_cast<std::int64_t>(block));
        q.integer(7,id);q.boolean(8,web);q.text(9,revision);q.integer(10,static_cast<std::int64_t>(html.size()));q.run();
        if(sqlite3_changes(db_)!=1)throw std::runtime_error("翻译缓存已失效或超过大小限制");
        exec(db_,"COMMIT");
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
void Store::set_translation_state(std::int64_t id,bool web,const std::string& revision,const std::string& state,const std::string& error) {
    if(state!="ready"&&state!="paused"&&state!="failed")throw std::runtime_error("无效的翻译状态");
    exec(db_,"BEGIN IMMEDIATE");
    try {
        const auto status=translation_status(id,web,revision);
        if(state=="ready"&&(status.total==0||status.completed!=status.total))throw std::runtime_error("文章仍有段落未翻译");
        if(state=="ready") {
            const auto source=web?fulltext_entry(id):entry(id);
            if(!source||translation_revision(*source)!=revision)throw std::runtime_error("原文已更新，本轮译文保留为旧版本");
        }
        Statement q(db_,"UPDATE article_translations SET state=?,error=? WHERE entry_id=? AND web=? AND revision=?");
        q.text(1,state);q.text(2,error);q.integer(3,id);q.boolean(4,web);q.text(5,revision);q.run();
        if(state=="ready") {
            Statement old(db_,"DELETE FROM article_translations WHERE entry_id=? AND web=? AND revision<>?");
            old.integer(1,id);old.boolean(2,web);old.text(3,revision);old.run();
        }
        exec(db_,"COMMIT");
    } catch(...) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<StoredSelection> Store::selections(std::int64_t entry_id,bool web,ReadingMode mode,const std::string& revision) const {
    const auto value=static_cast<int>(mode);
    if(entry_id<=0||value<0||value>2||revision.empty()||revision.size()>1024||revision.find('\0')!=std::string::npos)throw std::runtime_error("无效选区");
    Statement q(db_,"SELECT start_anchor,end_anchor,text,highlighted,translation FROM article_selections WHERE entry_id=? AND web=? AND mode=? AND revision=? ORDER BY start_anchor,end_anchor");
    q.integer(1,entry_id);q.boolean(2,web);q.integer(3,value);q.text(4,revision);
    std::vector<StoredSelection> result;int step;
    while((step=sqlite3_step(q.get()))==SQLITE_ROW){
        StoredSelection item;item.range.start=column_text(q.get(),0);item.range.end=column_text(q.get(),1);item.range.text=column_text(q.get(),2);
        item.highlighted=sqlite3_column_int(q.get(),3)!=0;item.translation=column_text(q.get(),4);result.push_back(std::move(item));
    }
    check(db_,step);return result;
}
std::optional<StoredSelection> Store::selection(const SelectionJob& job) const {
    validate_selection(job);
    Statement q(db_,"SELECT text,highlighted,translation FROM article_selections WHERE entry_id=? AND web=? AND mode=? AND revision=? AND start_anchor=? AND end_anchor=?");
    q.integer(1,job.entry_id);q.boolean(2,job.web);q.integer(3,static_cast<int>(job.mode));q.text(4,job.revision);q.text(5,job.range.start);q.text(6,job.range.end);
    const auto step=sqlite3_step(q.get());check(db_,step);if(step==SQLITE_DONE||column_text(q.get(),0)!=job.range.text)return std::nullopt;
    StoredSelection item;item.range=job.range;item.highlighted=sqlite3_column_int(q.get(),1)!=0;item.translation=column_text(q.get(),2);return item;
}
void Store::set_highlight(const SelectionJob& job,bool highlighted) {
    validate_selection(job);
    Statement q(db_,"INSERT INTO article_selections(entry_id,web,mode,revision,start_anchor,end_anchor,text,highlighted) VALUES(?,?,?,?,?,?,?,?) "
                    "ON CONFLICT(entry_id,web,mode,revision,start_anchor,end_anchor) DO UPDATE SET highlighted=excluded.highlighted WHERE text=excluded.text");
    q.integer(1,job.entry_id);q.boolean(2,job.web);q.integer(3,static_cast<int>(job.mode));q.text(4,job.revision);q.text(5,job.range.start);q.text(6,job.range.end);q.text(7,job.range.text);q.boolean(8,highlighted);q.run();
    if(sqlite3_changes(db_)!=1)throw std::runtime_error("选区锚点内容已变化");
}
void Store::save_selection_translation(const SelectionJob& job,const std::string& translated) {
    validate_selection(job);validate_selection_translation(translated);
    Statement q(db_,"INSERT INTO article_selections(entry_id,web,mode,revision,start_anchor,end_anchor,text,translation) VALUES(?,?,?,?,?,?,?,?) "
                    "ON CONFLICT(entry_id,web,mode,revision,start_anchor,end_anchor) DO UPDATE SET translation=excluded.translation WHERE text=excluded.text");
    q.integer(1,job.entry_id);q.boolean(2,job.web);q.integer(3,static_cast<int>(job.mode));q.text(4,job.revision);q.text(5,job.range.start);q.text(6,job.range.end);q.text(7,job.range.text);q.text(8,translated);q.run();
    if(sqlite3_changes(db_)!=1)throw std::runtime_error("选区锚点内容已变化");
}
void Store::set_position(std::int64_t id, std::int64_t offset, bool fulltext,ReadingMode mode) {
    const auto value=std::max<std::int64_t>(0,offset);
    if(mode!=ReadingMode::Original) {
        set_setting(std::string(mode==ReadingMode::Bilingual?"reader_position:bilingual:":"reader_position:translated:")+(fulltext?"web:":"rss:")+std::to_string(id),std::to_string(value));
        Statement global(db_,"UPDATE entries SET in_progress=1 WHERE id=?");global.integer(1,id);global.run();return;
    }
    if(fulltext){Statement q(db_,"UPDATE article_fulltext SET position=? WHERE entry_id=?");q.integer(1,value);q.integer(2,id);q.run();Statement global(db_,"UPDATE entries SET in_progress=1 WHERE id=?");global.integer(1,id);global.run();}
    else {Statement q(db_, "UPDATE entries SET position=?,in_progress=1 WHERE id=?"); q.integer(1,value); q.integer(2,id); q.run();}
}
void Store::begin_reading(std::int64_t id, std::int64_t offset, bool fulltext,ReadingMode mode) {
    exec(db_, "BEGIN IMMEDIATE");
    try {
        set_read(id, true);
        set_position(id, offset, fulltext,mode);
        set_setting("reader_content:" + std::to_string(id), fulltext ? "web" : "rss");
        if(mode!=ReadingMode::Original)set_setting(std::string("reader_mode:")+(fulltext?"web:":"rss:")+std::to_string(id),mode==ReadingMode::Bilingual?"bilingual":"translated");
        set_setting("last_entry", std::to_string(id));
        exec(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}
void Store::finish_reading(std::int64_t id) {
    Statement q(db_, "UPDATE entries SET in_progress=0,position=0,read=1 WHERE id=?");q.integer(1,id);q.run();
    Statement fulltext(db_,"UPDATE article_fulltext SET position=0 WHERE entry_id=?");fulltext.integer(1,id);fulltext.run();
    for(const auto* source:{"rss:","web:"})for(const auto* mode:{"translated:","bilingual:"})
        set_setting(std::string("reader_position:")+mode+source+std::to_string(id),"0");
}

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
