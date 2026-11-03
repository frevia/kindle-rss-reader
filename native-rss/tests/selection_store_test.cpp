#include "store.hpp"
#include <cassert>
#include <cstdio>
#include <sqlite3.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>

using namespace rss;

static SelectionJob job(std::int64_t id, bool web=false, ReadingMode mode=ReadingMode::Original,
                        std::string revision="rev-1", std::string text="selected") {
    SelectionJob value;value.entry_id=id;value.web=web;value.mode=mode;value.revision=std::move(revision);
    value.range={"start","end",std::move(text)};return value;
}

int main() {
    const auto path="/tmp/kindle-rss-selection-store-"+std::to_string(getpid())+".sqlite";
    std::remove(path.c_str());
    sqlite3* legacy=nullptr;assert(sqlite3_open(path.c_str(),&legacy)==SQLITE_OK);
    assert(sqlite3_exec(legacy,"CREATE TABLE feeds(id INTEGER PRIMARY KEY,url TEXT NOT NULL UNIQUE,title TEXT NOT NULL DEFAULT '',group_name TEXT NOT NULL DEFAULT '',error TEXT NOT NULL DEFAULT '',etag TEXT NOT NULL DEFAULT '',modified TEXT NOT NULL DEFAULT '',synced_at INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1); CREATE TABLE entries(id INTEGER PRIMARY KEY,feed_id INTEGER NOT NULL REFERENCES feeds(id),guid TEXT NOT NULL,title TEXT NOT NULL DEFAULT '',url TEXT NOT NULL DEFAULT '',author TEXT NOT NULL DEFAULT '',html TEXT NOT NULL DEFAULT '',text TEXT NOT NULL DEFAULT '',published INTEGER NOT NULL DEFAULT 0,read INTEGER NOT NULL DEFAULT 0,starred INTEGER NOT NULL DEFAULT 0,position INTEGER NOT NULL DEFAULT 0,UNIQUE(feed_id,guid)); CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL); INSERT INTO feeds(id,url) VALUES(1,'https://selection.test/feed'); INSERT INTO entries(id,feed_id,guid,title,text) VALUES(7,1,'old','Old','retained'); INSERT INTO settings VALUES('keep','legacy');",nullptr,nullptr,nullptr)==SQLITE_OK);
    sqlite3_close(legacy);
    {
        Store store(path);
        assert(store.entry(7)&&store.entry(7)->text=="retained"&&store.setting("keep")=="legacy");
        const auto rss=job(7), web=job(7,true), translated=job(7,false,ReadingMode::Translated), bilingual=job(7,false,ReadingMode::Bilingual), next_revision=job(7,false,ReadingMode::Original,"rev-2");
        store.set_highlight(rss,true);store.save_selection_translation(rss,"中文 & 原文");
        store.set_highlight(rss,false);
        auto saved=store.selection(rss);assert(saved&&!saved->highlighted&&saved->translation=="中文 & 原文");
        store.set_highlight(rss,true);store.save_selection_translation(rss,"另一译文");
        saved=store.selection(rss);assert(saved&&saved->highlighted&&saved->translation=="另一译文");
        store.set_highlight(web,true);store.set_highlight(translated,true);store.set_highlight(bilingual,true);store.set_highlight(next_revision,true);
        assert(store.selections(7,false,ReadingMode::Original,"rev-1").size()==1);
        assert(store.selections(7,true,ReadingMode::Original,"rev-1").size()==1);
        assert(store.selections(7,false,ReadingMode::Translated,"rev-1").size()==1);
        assert(store.selections(7,false,ReadingMode::Bilingual,"rev-1").size()==1);
        assert(store.selections(7,false,ReadingMode::Original,"rev-2").size()==1);
        auto wrong_text=rss;wrong_text.range.text="different";
        assert(!store.selection(wrong_text));
        bool rejected=false;try{store.set_highlight(wrong_text,false);}catch(const std::runtime_error&){rejected=true;}assert(rejected);

        const auto invalid=[&](SelectionJob bad){bool failed=false;try{store.set_highlight(bad,true);}catch(const std::runtime_error&){failed=true;}assert(failed);};
        auto bad=rss;bad.entry_id=0;invalid(bad);bad=rss;bad.mode=static_cast<ReadingMode>(9);invalid(bad);
        bad=rss;bad.revision.clear();invalid(bad);bad=rss;bad.revision=std::string(1025,'r');invalid(bad);
        bad=rss;bad.range.start.clear();invalid(bad);bad=rss;bad.range.end=std::string(4097,'e');invalid(bad);
        bad=rss;bad.range.text.clear();invalid(bad);bad=rss;bad.range.text=std::string(6001,'x');invalid(bad);
        bad=rss;bad.range.text="\xc0\xaf";invalid(bad);bad=rss;bad.range.text=std::string("a\0b",3);invalid(bad);
        bad=rss;bad.range.start=std::string("a\0b",3);invalid(bad);bad=rss;bad.range.end=bad.range.start;invalid(bad);
        bad=rss;bad.range.text=" \n\t";invalid(bad);bad=rss;bad.revision="\xc0\xaf";invalid(bad);
        const auto invalid_translation=[&](const std::string& text){bool failed=false;try{store.save_selection_translation(rss,text);}catch(const std::runtime_error&){failed=true;}assert(failed);};
        invalid_translation("");invalid_translation(" \n\t");invalid_translation(std::string(64*1024+1,'x'));invalid_translation("\xed\xa0\x80");invalid_translation(std::string("a\0b",3));
    }
    {
        Store reopened(path);
        const auto rss=job(7);auto saved=reopened.selection(rss);assert(saved&&saved->highlighted&&saved->translation=="另一译文");
        reopened.set_starred(7,true);reopened.remove_feed(1);
        saved=reopened.selection(rss);assert(saved&&saved->highlighted&&saved->translation=="另一译文");
        reopened.set_starred(7,false);reopened.set_read(7,true);assert(reopened.clean_read(100)==1);
        assert(reopened.selections(7,false,ReadingMode::Original,"rev-1").empty());
    }
    std::remove(path.c_str());
}
