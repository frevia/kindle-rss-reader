#include "home_bridge.hpp"

#include <sqlite3.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string_view>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace home_bridge {
namespace {
constexpr std::size_t kMaxFrame = 2048;
constexpr std::size_t kMaxOut = 64 * 1024;
volatile std::sig_atomic_t g_stop = 0;

struct Item { std::int64_t id = 0; std::string title, source; };
struct FeedCard { std::int64_t id = 0, unread = 0, entry_id = 0, published = 0; std::string title, latest_title, excerpt; };
struct Snapshot {
    std::string status = "unavailable", revision;
    std::int64_t unread = 0, feeds = 0, updated_at = 0;
    bool enabled = true;
    std::vector<Item> items;
    std::vector<FeedCard> feed_cards;
    std::string content, json;
};

std::string json_string(const std::string& input, std::size_t limit) {
    std::string clean;
    clean.reserve(std::min(input.size(), limit * 4));
    std::size_t chars = 0;
    for (std::size_t i = 0; i < input.size() && chars < limit;) {
        const auto c = static_cast<unsigned char>(input[i]);
        std::uint32_t cp = 0xfffd;
        std::size_t n = 1;
        if (c < 0x80) cp = c;
        else if (c >= 0xc2 && c <= 0xdf && i + 1 < input.size() && (static_cast<unsigned char>(input[i+1]) & 0xc0) == 0x80) {
            cp = ((c & 0x1f) << 6) | (static_cast<unsigned char>(input[i+1]) & 0x3f); n = 2;
        } else if (c >= 0xe0 && c <= 0xef && i + 2 < input.size() &&
                   (static_cast<unsigned char>(input[i+1]) & 0xc0) == 0x80 && (static_cast<unsigned char>(input[i+2]) & 0xc0) == 0x80) {
            const auto c1 = static_cast<unsigned char>(input[i+1]);
            if (!(c == 0xe0 && c1 < 0xa0) && !(c == 0xed && c1 >= 0xa0)) {
                cp = ((c & 0x0f) << 12) | ((c1 & 0x3f) << 6) | (static_cast<unsigned char>(input[i+2]) & 0x3f); n = 3;
            }
        } else if (c >= 0xf0 && c <= 0xf4 && i + 3 < input.size() &&
                   (static_cast<unsigned char>(input[i+1]) & 0xc0) == 0x80 && (static_cast<unsigned char>(input[i+2]) & 0xc0) == 0x80 && (static_cast<unsigned char>(input[i+3]) & 0xc0) == 0x80) {
            const auto c1 = static_cast<unsigned char>(input[i+1]);
            if (!(c == 0xf0 && c1 < 0x90) && !(c == 0xf4 && c1 >= 0x90)) {
                cp = ((c & 7) << 18) | ((c1 & 0x3f) << 12) | ((static_cast<unsigned char>(input[i+2]) & 0x3f) << 6) | (static_cast<unsigned char>(input[i+3]) & 0x3f); n = 4;
            }
        }
        i += n; ++chars;
        if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) cp = ' ';
        if (cp < 0x80) clean.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { clean.push_back(static_cast<char>(0xc0 | (cp >> 6))); clean.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else if (cp < 0x10000) { clean.push_back(static_cast<char>(0xe0 | (cp >> 12))); clean.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); clean.push_back(static_cast<char>(0x80 | (cp & 63))); }
        else { clean.push_back(static_cast<char>(0xf0 | (cp >> 18))); clean.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63))); clean.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63))); clean.push_back(static_cast<char>(0x80 | (cp & 63))); }
    }
    std::string out = "\"";
    for (unsigned char c : clean) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) { static constexpr char hex[] = "0123456789abcdef"; out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        else out.push_back(static_cast<char>(c));
    }
    out.push_back('"'); return out;
}

std::string normalized_excerpt(const std::string& input, const std::string& title) {
    const auto first_line_end = input.find_first_of("\r\n");
    std::size_t start = 0;
    std::size_t first_line_start = 0, first_line_stop = first_line_end == std::string::npos ? input.size() : first_line_end;
    while (first_line_start < first_line_stop && (input[first_line_start] == ' ' || input[first_line_start] == '\t' || input[first_line_start] == '\v' || input[first_line_start] == '\f')) ++first_line_start;
    while (first_line_stop > first_line_start && (input[first_line_stop - 1] == ' ' || input[first_line_stop - 1] == '\t' || input[first_line_stop - 1] == '\v' || input[first_line_stop - 1] == '\f')) --first_line_stop;
    if (!title.empty() && input.compare(first_line_start, first_line_stop - first_line_start, title) == 0) {
        start = first_line_end == std::string::npos ? input.size() : first_line_end + 1;
        if (first_line_end != std::string::npos && input[first_line_end] == '\r' && start < input.size() && input[start] == '\n') ++start;
    }
    std::string out;
    bool pending_space = false;
    for (std::size_t i = start; i < input.size(); ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            pending_space = !out.empty();
        } else {
            if (pending_space) out.push_back(' ');
            out.push_back(static_cast<char>(c));
            pending_space = false;
        }
    }
    return out;
}

std::string content_json(const Snapshot& s) {
    std::string out = "{\"type\":\"rss-home-v1\",\"status\":" + json_string(s.status, 32) +
        ",\"enabled\":" + std::string(s.enabled ? "true" : "false") +
        ",\"unread\":" + std::to_string(s.unread) + ",\"feeds\":" + std::to_string(s.feeds) +
        ",\"updatedAt\":" + std::to_string(s.updated_at) + ",\"items\":[";
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        if (i) out.push_back(',');
        out += "{\"id\":" + std::to_string(s.items[i].id) + ",\"title\":" + json_string(s.items[i].title, 48) +
               ",\"source\":" + json_string(s.items[i].source, 24) + "}";
    }
    out += "],\"feedCards\":[";
    for (std::size_t i = 0; i < s.feed_cards.size(); ++i) {
        if (i) out.push_back(',');
        const auto& card = s.feed_cards[i];
        out += "{\"id\":" + std::to_string(card.id) + ",\"title\":" + json_string(card.title, 24) +
               ",\"unread\":" + std::to_string(card.unread) + ",\"entryId\":" + std::to_string(card.entry_id) +
               ",\"latestTitle\":" + json_string(card.latest_title, 48) + ",\"excerpt\":" + json_string(card.excerpt, 160) +
               ",\"published\":" + std::to_string(card.published) + "}";
    }
    return out + "]}";
}

std::string with_revision(const Snapshot& s) {
    const auto marker = std::string("\"status\":");
    auto at = s.content.find(marker);
    if (at == std::string::npos) return s.content;
    return s.content.substr(0, at) + "\"revision\":" + json_string(s.revision, 64) + "," + s.content.substr(at);
}

std::string sha256(const std::string& data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32) throw std::runtime_error("digest unavailable");
    static constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
    for (unsigned int i = 0; i < length; ++i) { out.push_back(hex[digest[i] >> 4]); out.push_back(hex[digest[i] & 15]); }
    return out;
}

bool schema_has(sqlite3* db, const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA table_info(") + table + ")";
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &q, nullptr) != SQLITE_OK) return false;
    bool found = false; int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
        if (name && std::strcmp(name, column) == 0) found = true;
    }
    sqlite3_finalize(q); return rc == SQLITE_DONE && found;
}

bool query_one(sqlite3* db, const char* sql, std::int64_t& value) {
    sqlite3_stmt* q = nullptr; if (sqlite3_prepare_v2(db, sql, -1, &q, nullptr) != SQLITE_OK) return false;
    const int rc = sqlite3_step(q); const bool ok = rc == SQLITE_ROW;
    if (ok) value = sqlite3_column_int64(q, 0);
    const bool done = ok && sqlite3_step(q) == SQLITE_DONE; sqlite3_finalize(q); return done;
}

bool read_home_visible(sqlite3* db, bool& enabled) {
    const bool has_key = schema_has(db, "settings", "key");
    const bool has_value = schema_has(db, "settings", "value");
    if (!has_key && !has_value) return true;
    if (!has_key || !has_value) return false;
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key='home_visible'", -1, &q, nullptr) != SQLITE_OK) return false;
    const int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(q, 0));
        enabled = !(value && sqlite3_column_bytes(q, 0) == 1 && value[0] == '0');
    }
    const int final_rc = rc == SQLITE_ROW ? sqlite3_step(q) : rc;
    const bool ok = (rc == SQLITE_ROW || rc == SQLITE_DONE) && final_rc == SQLITE_DONE;
    sqlite3_finalize(q);
    return ok;
}

bool prepare_open_links(const std::string& path, const Snapshot& snapshot) {
    if (path.empty()) return true;
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return false;
    struct stat info{};
    bool ok = fstat(directory, &info) == 0 && info.st_uid == geteuid();
    static std::uint64_t serial = 0;
    for (const auto& card : snapshot.feed_cards) {
        if (!ok) break;
        if (!card.entry_id) continue;
        if (card.entry_id < 0 || card.entry_id > 9007199254740991LL) { ok = false; break; }
        const std::string id = std::to_string(card.entry_id), name = "entry-" + id + ".sh";
        const std::string content = "#!/bin/sh\n# Name: RSS Article\n# Author: RSS Reader\n"
            "# Icon: /mnt/us/native-rss/book-cover.png\n# DontUseFBInk\n"
            "exec sh /mnt/us/native-rss/launch.sh --article " + id + "\n";
        int existing = openat(directory, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (existing >= 0) {
            struct stat st{}; std::array<char, 512> buffer{};
            const bool regular = fstat(existing, &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_uid == geteuid() && st.st_size == static_cast<off_t>(content.size());
            const auto length = regular ? read(existing, buffer.data(), buffer.size()) : -1;
            close(existing);
            ok = regular && length == static_cast<ssize_t>(content.size()) &&
                std::string(buffer.data(), static_cast<std::size_t>(length)) == content;
            continue;
        }
        if (errno != ENOENT) { ok = false; break; }
        const std::string temp = ".entry-" + id + "." + std::to_string(getpid()) + "." + std::to_string(++serial);
        int fd = openat(directory, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0755);
        if (fd < 0) { ok = false; break; }
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto n = write(fd, content.data() + offset, content.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += static_cast<std::size_t>(n);
        }
        ok = offset == content.size() && fchmod(fd, 0755) == 0 && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok && renameat(directory, temp.c_str(), directory, name.c_str()) != 0) ok = false;
        if (!ok) unlinkat(directory, temp.c_str(), 0);
    }
    close(directory);
    return ok;
}

Snapshot read_snapshot(const std::string& path, const std::string& open_dir = {}) {
    Snapshot s;
    struct stat st{};
    if (path.empty() || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { s.content = content_json(s); s.revision = sha256(s.content); s.json = with_revision(s); return s; }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { if (db) sqlite3_close(db); s.content = content_json(s); s.revision = sha256(s.content); s.json = with_revision(s); return s; }
    sqlite3_busy_timeout(db, 150);
    bool ok = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
    ok = ok && schema_has(db, "feeds", "id") && schema_has(db, "feeds", "title") && schema_has(db, "feeds", "active") && schema_has(db, "feeds", "synced_at") &&
         schema_has(db, "entries", "id") && schema_has(db, "entries", "feed_id") && schema_has(db, "entries", "title") && schema_has(db, "entries", "read") && schema_has(db, "entries", "published");
    if (ok) ok = read_home_visible(db, s.enabled);
    if (ok) {
        ok = query_one(db, "SELECT count(*) FROM entries WHERE read=0", s.unread) &&
             query_one(db, "SELECT count(*) FROM feeds WHERE active=1", s.feeds) &&
             query_one(db, "SELECT coalesce(max(synced_at),0) FROM feeds WHERE active=1", s.updated_at);
    }
    if (ok) {
        sqlite3_stmt* q = nullptr;
        const std::string sql = std::string("SELECT e.id,substr(e.title,1,192),substr(f.title,1,96) FROM entries e JOIN feeds f ON f.id=e.feed_id ") +
            (s.unread ? "WHERE e.read=0 " : "") + "ORDER BY e.published DESC,e.id DESC LIMIT 2";
        ok = sqlite3_prepare_v2(db, sql.c_str(), -1, &q, nullptr) == SQLITE_OK;
        if (ok) {
            int rc;
            while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
                Item item; item.id = sqlite3_column_int64(q, 0);
                const auto* title = sqlite3_column_text(q, 1); const auto title_bytes = sqlite3_column_bytes(q, 1);
                const auto* source = sqlite3_column_text(q, 2); const auto source_bytes = sqlite3_column_bytes(q, 2);
                if (title && title_bytes > 0) item.title.assign(reinterpret_cast<const char*>(title), static_cast<std::size_t>(title_bytes));
                if (source && source_bytes > 0) item.source.assign(reinterpret_cast<const char*>(source), static_cast<std::size_t>(source_bytes));
                s.items.push_back(std::move(item));
            }
            ok = rc == SQLITE_DONE;
        }
        sqlite3_finalize(q);
    }
    if (ok) {
        sqlite3_stmt* q = nullptr;
        const bool has_entry_text = schema_has(db, "entries", "text");
        const std::string sql = std::string("SELECT f.id,substr(f.title,1,96),") +
            "(SELECT count(*) FROM entries WHERE feed_id=f.id AND read=0),"
            "coalesce(e.id,0),coalesce(substr(e.title,1,192),''),coalesce(e.published,0)," +
            (has_entry_text ? "coalesce(substr(e.text,1,2048),'') " : "'' ") + "FROM feeds f "
            "LEFT JOIN entries e ON e.id=(SELECT id FROM entries WHERE feed_id=f.id "
            "ORDER BY CASE WHEN read=0 THEN 0 ELSE 1 END,published DESC,id DESC LIMIT 1) "
            "WHERE f.active=1 "
            "ORDER BY (SELECT max(published) FROM entries WHERE feed_id=f.id) DESC,f.id ASC LIMIT 3";
        ok = sqlite3_prepare_v2(db, sql.c_str(), -1, &q, nullptr) == SQLITE_OK;
        if (ok) {
            int rc;
            while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
                FeedCard card; card.id = sqlite3_column_int64(q, 0); card.unread = sqlite3_column_int64(q, 2);
                card.entry_id = sqlite3_column_int64(q, 3);
                const auto selected_published = sqlite3_column_int64(q, 5);
                if (selected_published >= 0 && selected_published <= 8640000000000LL) card.published = selected_published;
                const auto* title = sqlite3_column_text(q, 1); const auto title_bytes = sqlite3_column_bytes(q, 1);
                const auto* latest = sqlite3_column_text(q, 4); const auto latest_bytes = sqlite3_column_bytes(q, 4);
                const auto* excerpt = sqlite3_column_text(q, 6); const auto excerpt_bytes = sqlite3_column_bytes(q, 6);
                if (title && title_bytes > 0) card.title.assign(reinterpret_cast<const char*>(title), static_cast<std::size_t>(title_bytes));
                if (latest && latest_bytes > 0) card.latest_title.assign(reinterpret_cast<const char*>(latest), static_cast<std::size_t>(latest_bytes));
                if (excerpt && excerpt_bytes > 0) card.excerpt = normalized_excerpt(std::string(reinterpret_cast<const char*>(excerpt), static_cast<std::size_t>(excerpt_bytes)), card.latest_title);
                s.feed_cards.push_back(std::move(card));
            }
            ok = rc == SQLITE_DONE;
        }
        if (q && sqlite3_finalize(q) != SQLITE_OK) ok = false;
    }
    if (ok) ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    else sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (ok) ok = prepare_open_links(open_dir, s);
    if (ok) s.status = (s.unread == 0 && s.items.empty()) ? "empty" : "ready";
    else { s = Snapshot{}; }
    s.content = content_json(s); s.revision = sha256(s.content); s.json = with_revision(s); return s;
}

bool session_reading(const std::string& path) {
    if (path.empty()) return false;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno != ENOENT;
    const bool locked = flock(fd, LOCK_SH | LOCK_NB) != 0;
    if (!locked) flock(fd, LOCK_UN);
    close(fd); return locked;
}

struct State { bool ready = false; std::size_t clients = 0; std::uint64_t acks = 0; std::string revision; };
void write_state(const std::string& path, const State& state, std::string& previous) {
    if (path.empty()) return;
    struct stat st{}; if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return;
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    const std::string data = "pid=" + std::to_string(static_cast<long long>(getpid())) + "\nready=" + (state.ready ? "1" : "0") +
        "\nclients=" + std::to_string(state.clients) + "\nrevision=" + state.revision + "\nacks=" + std::to_string(state.acks) + "\n";
    if (data == previous) return;
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    std::size_t off = 0;
    while (off < data.size()) { const auto n = write(fd, data.data() + off, data.size() - off); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; off += static_cast<std::size_t>(n); }
    const bool complete = off == data.size() && fsync(fd) == 0;
    close(fd);
    if (complete && rename(tmp.c_str(), path.c_str()) == 0) previous = data;
    else unlink(tmp.c_str());
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    return s;
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32); return s; }
bool token_has(const std::string& value, const std::string& token) {
    std::size_t start = 0;
    while (start <= value.size()) { const auto end = value.find(',', start); if (lower(trim(value.substr(start, end == std::string::npos ? end : end-start))) == token) return true; if (end == std::string::npos) break; start = end + 1; }
    return false;
}

std::string ws_frame(unsigned char opcode, const std::string& payload) {
    std::string out; out.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) out.push_back(static_cast<char>(payload.size()));
    else { out.push_back(126); out.push_back(static_cast<char>((payload.size() >> 8) & 255)); out.push_back(static_cast<char>(payload.size() & 255)); }
    out += payload; return out;
}

struct Client {
    int fd = -1; bool upgraded = false, closing = false, sent_snapshot = false;
    std::string input, output; std::size_t sent = 0;
    std::chrono::steady_clock::time_point deadline, heartbeat;
};

void fail_http(Client& c, int code, const char* text) {
    const std::string body = std::string(text) + "\n";
    c.output = "HTTP/1.1 " + std::to_string(code) + " " + (code == 403 ? "Forbidden" : "Bad Request") + "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    c.closing = true;
}

bool handshake(Client& c) {
    const auto end = c.input.find("\r\n\r\n");
    if (end == std::string::npos) return c.input.size() <= 8192;
    if (end + 4 > 8192) { fail_http(c, 400, "bad handshake"); return false; }
    std::string request = c.input.substr(0, end); c.input.erase(0, end + 4);
    const auto line_end = request.find("\r\n");
    if (line_end == std::string::npos || request.substr(0, line_end) != "GET /rss-home/v1 HTTP/1.1") { fail_http(c, 400, "bad request"); return false; }
    std::map<std::string,std::string> headers;
    std::size_t pos = line_end + 2;
    while (pos < request.size()) {
        const auto e = request.find("\r\n", pos); const auto stop = e == std::string::npos ? request.size() : e;
        const auto colon = request.find(':', pos);
        if (colon == std::string::npos || colon >= stop) { fail_http(c, 400, "bad headers"); return false; }
        auto name = lower(trim(request.substr(pos, colon-pos))); auto value = trim(request.substr(colon+1, stop-colon-1));
        if (name.empty() || headers.count(name)) { fail_http(c, 400, "bad headers"); return false; }
        headers.emplace(name, value); pos = stop + 2;
    }
    if (headers.count("origin")) { fail_http(c, 403, "origin not allowed"); return false; }
    if (lower(headers["upgrade"]) != "websocket" || !token_has(headers["connection"], "upgrade") || headers["sec-websocket-version"] != "13") { fail_http(c, 400, "websocket required"); return false; }
    const auto key_it = headers.find("sec-websocket-key"); if (key_it == headers.end() || key_it->second.size() != 24 || key_it->second.substr(22) != "==") { fail_http(c, 400, "bad websocket key"); return false; }
    std::array<unsigned char,32> decoded{};
    const int decoded_n = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(key_it->second.data()), static_cast<int>(key_it->second.size()));
    std::array<unsigned char,25> canonical{};
    if (decoded_n != 18 || EVP_EncodeBlock(canonical.data(), decoded.data(), 16) != 24 || std::memcmp(canonical.data(), key_it->second.data(), 24) != 0) { fail_http(c, 400, "bad websocket key"); return false; }
    std::string material = key_it->second + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{}; unsigned int digest_len = 0;
    if (EVP_Digest(material.data(), material.size(), digest.data(), &digest_len, EVP_sha1(), nullptr) != 1 || digest_len != 20) { fail_http(c, 400, "bad websocket key"); return false; }
    std::array<unsigned char,32> b64{}; const int b64len = EVP_EncodeBlock(b64.data(), digest.data(), 20);
    c.output = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + std::string(reinterpret_cast<char*>(b64.data()), static_cast<std::size_t>(b64len)) + "\r\n\r\n";
    c.upgraded = true; return true;
}

bool parse_frames(Client& c, const Snapshot& current, State& state, bool lock_clear) {
    if (c.sent) { c.output.erase(0, c.sent); c.sent = 0; }
    while (c.input.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(c.input[0]), b1 = static_cast<unsigned char>(c.input[1]);
        if ((b0 & 0x70) || !(b0 & 0x80) || !(b1 & 0x80)) return false;
        const unsigned char op = b0 & 15; std::uint64_t n = b1 & 127; std::size_t header = 2;
        if (n == 126) { if (c.input.size() < 4) return true; n = (static_cast<unsigned char>(c.input[2]) << 8) | static_cast<unsigned char>(c.input[3]); header = 4; if (n < 126) return false; }
        else if (n == 127) { return false; }
        if (n > kMaxFrame || (op >= 8 && (n > 125 || !(b0 & 0x80)))) return false;
        if (c.input.size() < header + 4 + n) return true;
        std::array<unsigned char,4> mask{}; for (unsigned i=0;i<4;++i) mask[i]=static_cast<unsigned char>(c.input[header+i]);
        std::string payload(c.input.data()+header+4, static_cast<std::size_t>(n));
        for (std::size_t i=0;i<payload.size();++i) payload[i]=static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i%4]);
        c.input.erase(0, header+4+static_cast<std::size_t>(n));
        if (op == 9) c.output += ws_frame(10, payload);
        else if (op == 10) { continue; }
        else if (op == 8) { if (payload.size() == 1) return false; c.output += ws_frame(8, payload); c.closing = true; return true; }
        else if (op == 1) {
            if (payload.size() > kMaxFrame) return false;
            const std::string prefix = "{\"type\":\"rss-home-ack\",\"revision\":\"";
            if (payload.size() == prefix.size()+64+2 && payload.compare(0,prefix.size(),prefix)==0 && payload.compare(prefix.size()+64,2,"\"}")==0) {
                const auto rev = payload.substr(prefix.size(),64);
                const bool hex = std::all_of(rev.begin(),rev.end(),[](char x){return (x>='0'&&x<='9')||(x>='a'&&x<='f');});
                if (lock_clear && hex && rev == current.revision) { state.ready = true; ++state.acks; }
            } else return false;
        } else return false;
        if (c.output.size() - c.sent > kMaxOut) return false;
    }
    return true;
}

void nonblock(int fd) { const int flags = fcntl(fd, F_GETFL, 0); if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK); }
void close_client(Client& c) { if (c.fd >= 0) close(c.fd); c.fd = -1; }

class DatabaseWatcher {
public:
    ~DatabaseWatcher() { if (db_) sqlite3_close_v2(db_); }
    void clear() { reset(); }
    bool changed(const std::string& path) {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { reset(); return true; }
        if (!db_ || st.st_dev != device_ || st.st_ino != inode_) {
            reset();
            if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { reset(); return true; }
            sqlite3_busy_timeout(db_, 150); device_ = st.st_dev; inode_ = st.st_ino;
            return read_version();
        }
        return read_version();
    }
private:
    void reset() { if (db_) sqlite3_close_v2(db_); db_ = nullptr; device_ = 0; inode_ = 0; known_ = false; }
    bool read_version() {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA data_version", -1, &q, nullptr) != SQLITE_OK) { reset(); return true; }
        const int rc = sqlite3_step(q); const int value = rc == SQLITE_ROW ? sqlite3_column_int(q, 0) : -1;
        sqlite3_finalize(q);
        if (value < 0) { reset(); return true; }
        const bool changed = !known_ || version_ != value; known_ = true; version_ = value; return changed;
    }
    sqlite3* db_ = nullptr; dev_t device_ = 0; ino_t inode_ = 0; int version_ = 0; bool known_ = false;
};
} // namespace

int run_summary(const Options& options) {
    Snapshot s;
    if (!session_reading(options.session_lock)) s = read_snapshot(options.database);
    if (s.content.empty()) { s.content = content_json(s); s.revision = sha256(s.content); s.json = with_revision(s); }
    std::cout << s.json << '\n'; return 0;
}

int run_server(const Options& options) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0); if (listen_fd < 0) return 1;
    int yes = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); nonblock(listen_fd);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(options.port);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 8) != 0) { close(listen_fd); return 1; }
    socklen_t alen = sizeof(addr); if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) { close(listen_fd); return 1; }
    g_stop = 0;
    std::signal(SIGINT, [](int){g_stop=1;}); std::signal(SIGTERM, [](int){g_stop=1;});
    std::cout << "{\"event\":\"listening\",\"port\":" << ntohs(addr.sin_port) << "}\n" << std::flush;
    Snapshot current; current.content = content_json(current); current.revision = sha256(current.content); current.json = with_revision(current);
    State state; state.revision = current.revision;
    std::vector<Client> clients; DatabaseWatcher watcher;
    auto next_check = std::chrono::steady_clock::now();
    bool retry_read = false, was_reading = false;
    std::string saved_state;
    write_state(options.state, state, saved_state);
    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        const bool reading = session_reading(options.session_lock);
        if (was_reading && !reading) for (auto& c : clients) c.sent_snapshot = false;
        was_reading = reading;
        const bool pending = std::any_of(clients.begin(), clients.end(), [](const Client& c){return c.upgraded && !c.closing && !c.sent_snapshot;});
        const bool connected = std::any_of(clients.begin(), clients.end(), [](const Client& c){return c.upgraded && !c.closing;});
        if (connected && !reading && (pending || now >= next_check)) {
            const bool changed = watcher.changed(options.database);
            if (pending || retry_read || changed) {
                Snapshot fresh = read_snapshot(options.database, options.open_dir);
                retry_read = fresh.status == "unavailable";
                if (!retry_read || current.status == "unavailable") {
                    if (fresh.revision != current.revision) {
                        current = std::move(fresh); state.ready = false; state.revision = current.revision;
                        for (auto& c : clients) c.sent_snapshot = false;
                    }
                }
            }
            for (auto& c : clients) if (c.fd >= 0 && c.upgraded && !c.closing && !c.sent_snapshot) {
                if (c.sent) { c.output.erase(0, c.sent); c.sent = 0; }
                const auto frame = ws_frame(1, current.json);
                if (c.output.size() - c.sent + frame.size() > kMaxOut) { close_client(c); continue; }
                c.output += frame; c.sent_snapshot = true;
                c.heartbeat = now + std::chrono::seconds(15);
            }
            next_check = now + std::chrono::seconds(2);
        }
        if (!reading) for (auto& c : clients) {
            if (c.fd < 0 || !c.upgraded || !c.sent_snapshot || c.closing || now < c.heartbeat) continue;
            if (c.sent) { c.output.erase(0, c.sent); c.sent = 0; }
            const auto frame = ws_frame(1, "{\"type\":\"rss-home-heartbeat\"}");
            if (c.output.size() + frame.size() > kMaxOut) { close_client(c); continue; }
            c.output += frame;
            c.heartbeat = now + std::chrono::seconds(15);
        }
        if (reading || !connected) state.ready = false;
        std::vector<pollfd> pfds; pfds.push_back({listen_fd, POLLIN, 0});
        for (const auto& c : clients) if (c.fd >= 0) pfds.push_back({c.fd, static_cast<short>(POLLIN | ((!c.output.empty() && (!reading || !c.sent_snapshot)) ? POLLOUT : 0)), 0});
        const int pr = poll(pfds.data(), pfds.size(), 1000);
        if (pr < 0 && errno != EINTR) break;
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int fd = accept(listen_fd, nullptr, nullptr); if (fd < 0) break;
                nonblock(fd); if (clients.size() >= 4) { close(fd); continue; }
                Client c; c.fd = fd; c.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); clients.push_back(std::move(c));
            }
        }
        std::size_t pi = 1;
        for (auto& c : clients) {
            if (c.fd < 0) continue;
            const short events = pi < pfds.size() ? pfds[pi++].revents : 0;
            if (events & (POLLERR | POLLHUP | POLLNVAL)) { close_client(c); continue; }
            if (events & POLLIN) {
                char buf[4096]; const auto got = recv(c.fd, buf, sizeof(buf), 0);
                if (got <= 0) { if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) close_client(c); }
                else {
                    if (c.input.size() + static_cast<std::size_t>(got) > (c.upgraded ? kMaxFrame + 14 : 8192)) close_client(c);
                    else { c.input.append(buf, static_cast<std::size_t>(got));
                        if (!c.upgraded) {
                            if (!handshake(c) && c.closing) { }
                            else if (c.upgraded) { }
                        }
                        if (c.upgraded && !c.closing && !parse_frames(c, current, state, !session_reading(options.session_lock))) { c.output += ws_frame(8, std::string("\x03\xea",2)); c.closing = true; }
                    }
                }
            }
            if (c.fd >= 0 && (events & POLLOUT || !c.output.empty()) &&
                (!c.sent_snapshot || !session_reading(options.session_lock))) {
                const auto remain = c.output.size() - c.sent;
                if (remain) { const auto sent = send(c.fd, c.output.data() + c.sent, std::min<std::size_t>(remain, 8192), MSG_NOSIGNAL); if (sent > 0) c.sent += static_cast<std::size_t>(sent); else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) close_client(c); }
                if (c.fd >= 0 && c.sent == c.output.size()) { c.output.clear(); c.sent = 0; if (c.closing) close_client(c); }
            }
            if (c.fd >= 0 && !c.upgraded && std::chrono::steady_clock::now() >= c.deadline) close_client(c);
        }
        clients.erase(std::remove_if(clients.begin(), clients.end(), [](const Client& c){return c.fd < 0;}), clients.end());
        state.clients = clients.size();
        if (clients.empty()) { state.ready = false; watcher.clear(); }
        write_state(options.state, state, saved_state);
    }
    for (auto& c : clients) close_client(c);
    close(listen_fd); state.ready = false; state.clients = 0; write_state(options.state, state, saved_state);
    return 0;
}

int run_cli(int argc, char** argv) {
    Options options; std::string command;
    for (int i=1;i<argc;++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string { if (i+1 >= argc) throw std::runtime_error(std::string("missing value for ")+name); return argv[++i]; };
        try {
            if (arg == "--db") options.database = value("--db");
            else if (arg == "--session-lock") options.session_lock = value("--session-lock");
            else if (arg == "--state") options.state = value("--state");
            else if (arg == "--open-dir") options.open_dir = value("--open-dir");
            else if (arg == "--port") { const auto raw = value("--port"); std::size_t consumed = 0; const auto n = std::stoul(raw, &consumed); if (consumed != raw.size() || n > 65535) throw std::runtime_error("bad port"); options.port = static_cast<std::uint16_t>(n); }
            else if (arg == "summary" || arg == "serve") { if (!command.empty()) throw std::runtime_error("one command required"); command = arg; }
            else throw std::runtime_error("unknown argument");
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
    }
    if (command == "summary") return run_summary(options);
    if (command == "serve") {
        // Keep installed Upstart jobs compatible while enabling article-specific scriptlets.
        if (options.open_dir.empty() && options.database == "/mnt/us/native-rss/data/rss.sqlite")
            options.open_dir = "/mnt/us/native-rss/home/open";
        return run_server(options);
    }
    std::cerr << "usage: kindle-rss-home [--db FILE] [--port N] [--session-lock FILE] [--state FILE] [--open-dir DIR] summary|serve\n";
    return 2;
}

} // namespace home_bridge
