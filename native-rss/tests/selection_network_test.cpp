#include "net.hpp"
#include "translation.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace rss;

int main(int argc,char** argv) {
    assert(argc==3);const std::string endpoint=argv[1],scenario=argv[2];
    const auto path="/tmp/kindle-rss-selection-net-"+std::to_string(getpid())+".sqlite";std::remove(path.c_str());
    Store store(path);Feed feed;feed.url="https://selection.test/feed";const auto feed_id=store.add_feed(feed);
    Entry article;article.guid="selected";article.title="Article title";article.url="https://selection.test/article";
    article.html="<p>Full article body</p>";article.text="Full article body";
    ParsedFeed parsed;parsed.entries={article};store.save_entries(feed_id,parsed);const auto id=store.entries(View::All,feed_id).front().id;
    const auto source=store.entry(id);assert(source);
    SelectionJob job;job.entry_id=id;job.mode=ReadingMode::Original;job.revision=translation_revision(*source);
    job.range={"byte:0","byte:14","Selected phrase."};
    QwenConfig config;config.api_key="fake-selection-key";config.endpoint=endpoint;
    std::atomic<bool> cancel{false};
    if(scenario=="stale") {
        article.text="Changed article body";article.html="<p>Changed article body</p>";ParsedFeed changed;changed.entries={article};store.save_entries(feed_id,changed);
    }
    if(scenario=="success-cache") {
        store.set_highlight(job,true);
        const auto first=translate_selection(store,job,config,cancel);
        config.api_key.clear();
        const auto second=translate_selection(store,job,config,cancel);
        assert(first==second&&first=="译文 & 原文");const auto cached=store.selection(job);
        assert(cached&&cached->highlighted&&cached->translation==first);std::cout<<first<<"\n";
    } else if(scenario=="failure") {
        store.set_highlight(job,true);bool failed=false;try{translate_selection(store,job,config,cancel);}catch(const std::runtime_error&){failed=true;}
        const auto cached=store.selection(job);assert(failed&&cached&&cached->highlighted&&cached->translation.empty());
    } else if(scenario=="cancel") {
        store.set_highlight(job,true);std::thread stop([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel=true;});
        bool failed=false;try{translate_selection(store,job,config,cancel);}catch(const std::runtime_error&){failed=true;}stop.join();
        const auto cached=store.selection(job);assert(failed&&cached&&cached->highlighted&&cached->translation.empty());
    } else if(scenario=="cancel-before") {
        store.set_highlight(job,true);cancel=true;bool failed=false;try{translate_selection(store,job,config,cancel);}catch(const std::runtime_error&){failed=true;}
        const auto cached=store.selection(job);assert(failed&&cached&&cached->highlighted&&cached->translation.empty());
    } else if(scenario=="stale") {
        bool failed=false;try{translate_selection(store,job,config,cancel);}catch(const std::runtime_error&){failed=true;}
        assert(failed&&!store.selection(job));
    } else assert(false);
    std::remove(path.c_str());return 0;
}
