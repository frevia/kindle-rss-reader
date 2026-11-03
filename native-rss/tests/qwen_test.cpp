#include "qwen.hpp"
#include "net.hpp"
#include "translation.hpp"
#include "feed.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <unistd.h>

template<class F> void rejects(F action) {bool failed=false;try{action();}catch(const std::exception&){failed=true;}assert(failed);}
int main(int argc,char** argv) {
    using namespace rss;
    const auto folder=std::filesystem::temp_directory_path()/("rss-qwen-test-"+std::to_string(getpid()));
    std::filesystem::create_directories(folder);
    const auto path=(folder/"rss.sqlite").string();
    unsetenv("RSS_QWEN_API_KEY");unsetenv("RSS_QWEN_ENDPOINT");
    rejects([&]{qwen_config(path);});
    {std::ofstream out(folder/"translation.conf");out<<"\xef\xbb\xbf# local configuration\nRSS_QWEN_API_KEY=local-test-key\nRSS_QWEN_ENDPOINT=http://127.0.0.1:1234/translate\n";}
    auto config=qwen_config(path);assert(config.api_key=="local-test-key");
    assert(config.endpoint=="http://127.0.0.1:1234/translate");
    setenv("RSS_QWEN_API_KEY","environment-test-key",1);
    assert(qwen_config(path).api_key=="environment-test-key");
    {std::ofstream out(folder/"translation.conf");out<<"RSS_QWEN_API_KEY=changed-test-key\n";}
    unsetenv("RSS_QWEN_API_KEY");assert(qwen_config(path).api_key=="changed-test-key");
    const auto valid=config;
    for(const auto* endpoint:{"http://example.com/api","file:///etc/passwd","https://name:secret@example.com/api","https://example.com/api?key=secret","https://example.com/api#fragment","https://example.com/\r\n"}) {
        config=valid;config.endpoint=endpoint;rejects([&]{validate_qwen_config(config);});
    }
    config=valid;config.api_key="test\r\nX-Other: bad";rejects([&]{validate_qwen_config(config);});
    {std::ofstream out(folder/"translation.conf");out<<"OTHER_VARIABLE=not-accepted\n";}
    rejects([&]{qwen_config(path);});
    const auto request=qwen_request("Quote \" and slash \\ and\nnewline");
    assert(request.find("qwen-mt-flash")!=std::string::npos&&request.find("\\n")==std::string::npos&&request.find("\\u000a")!=std::string::npos);
    assert(request.find("source_lang\":\"English")!=std::string::npos);
    assert(request.find("HTML fragment:")==std::string::npos);
    for(const auto* raw:{"A <strong>quiet</strong> reader.","Keep RSSKEEP123X0Q private."}) {
        const auto plain_request=qwen_request(raw);
        assert(plain_request.find("translation_options")!=std::string::npos);
        assert(plain_request.find(R"("source_lang":"English")")!=std::string::npos);
        assert(plain_request.find(R"("target_lang":"Chinese")")!=std::string::npos);
        assert(plain_request.find(raw)!=std::string::npos);
        assert(plain_request.find("\"terms\"")==std::string::npos);
        assert(plain_request.find("HTML fragment:")==std::string::npos);
        assert(plain_request.find("English to Simplified Chinese")==std::string::npos);
    }
    const std::string protected_token="RSSKEEP123X0Q";
    const auto protected_request=qwen_request("Keep "+protected_token,{protected_token});
    assert(protected_request.find(R"("source_lang":"English")")!=std::string::npos);
    assert(protected_request.find(R"("target_lang":"Chinese")")!=std::string::npos);
    assert(protected_request.find(R"("terms":[{"source":"RSSKEEP123X0Q","target":"RSSKEEP123X0Q"}])")!=std::string::npos);
    assert(protected_request.find("Keep "+protected_token)!=std::string::npos);
    assert(protected_request.find("<code")==std::string::npos&&protected_request.find("<pre")==std::string::npos&&protected_request.find("href=")==std::string::npos);
    assert(protected_request.find("HTML fragment:")==std::string::npos);
    assert(qwen_translation(R"({"choices":[{"message":{"content":"中文\n第二行"},"finish_reason":"stop"}]})")=="中文\n第二行");
    for(const auto* response:{"not json",R"({"choices":[]})",R"({"choices":[{"message":{"content":"partial"},"finish_reason":"length"}]})",R"({"choices":[{"message":{"content":5},"finish_reason":"stop"}]})",R"({"choices":[{"message":{"content":"\u0000"},"finish_reason":"stop"}]})"})
        rejects([&]{qwen_translation(response);});
    rejects([&]{qwen_translation(std::string(256*1024+1,'x'));});
    rejects([&]{qwen_request(std::string(6001,'x'));});

    Store store(path);Feed feed;feed.url="https://example.invalid/feed";const auto fid=store.add_feed(feed);
    Entry source;source.guid="one";source.title="English title";source.html="<p>A quiet reader.</p><p>Offline reading.</p>";source.text=html_to_text(source.html);
    ParsedFeed parsed;parsed.entries={source};store.save_entries(fid,parsed);source=*store.entry(store.entries(View::All).front().id);
    const auto doc=translation_document(source);assert(doc.blocks.size()>=3);
    store.begin_translation(source.id,false,doc.revision,doc.blocks.size());
    store.save_translation_block(source.id,false,doc.revision,0,"中文标题");
    assert(store.translation_status(source.id,false,doc.revision).completed==1);
    sqlite3* raw=nullptr;assert(sqlite3_open(path.c_str(),&raw)==SQLITE_OK);
    assert(sqlite3_exec(raw,"CREATE TRIGGER fail_translation BEFORE INSERT ON article_translation_blocks BEGIN SELECT RAISE(FAIL,'write failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
    rejects([&]{store.save_translation_block(source.id,false,doc.revision,1,"不可写入");});
    assert(store.translation_status(source.id,false,doc.revision).completed==1);
    assert(store.entry(source.id)->html==source.html&&!store.entry(source.id)->read);
    assert(sqlite3_exec(raw,"DROP TRIGGER fail_translation",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(raw);
    store.set_translation_state(source.id,false,doc.revision,"paused");
    store.begin_translation(source.id,false,doc.revision,doc.blocks.size());
    assert(store.translation_blocks(source.id,false,doc.revision)[0]=="中文标题");
    rejects([&]{store.set_translation_state(source.id,false,doc.revision,"ready");});
    std::atomic<bool> cancel{true};SyncProgress last;
    translate_article(store,source.id,false,valid,cancel,[&](const SyncProgress& p){last=p;});
    assert(store.translation_status(source.id,false,doc.revision).state=="paused"&&last.failed==0);
    assert(store.translation_status(source.id,false,doc.revision).completed==1);
    if(argc==2) {
        auto slow=valid;slow.endpoint=argv[1];cancel=false;
        std::thread stop([&]{std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel=true;});
        try{translate_article(store,source.id,false,slow,cancel,[&](const SyncProgress& p){last=p;});}
        catch(...){stop.join();throw;}
        stop.join();
        assert(store.translation_status(source.id,false,doc.revision).state=="paused");
        assert(store.translation_status(source.id,false,doc.revision).completed==1);
    }
    for(std::size_t i=1;i<doc.blocks.size();++i)store.save_translation_block(source.id,false,doc.revision,i,"段落译文");
    store.set_translation_state(source.id,false,doc.revision,"ready");
    const auto ready=store.translation_status(source.id,false,doc.revision);assert(ready.state=="ready"&&ready.completed==ready.total);
    source.html+="<p>New text.</p>";source.text=html_to_text(source.html);parsed.entries={source};store.save_entries(fid,parsed);
    const auto newer=translation_document(*store.entry(source.id));
    assert(store.translation_status(source.id,false,newer.revision).state=="stale");
    rejects([&]{store.save_translation_block(source.id,false,doc.revision,0,"不能覆盖");});
    store.begin_translation(source.id,false,newer.revision,newer.blocks.size());
    store.set_translation_state(source.id,false,newer.revision,"failed","temporary failure");
    assert(store.translation_blocks(source.id,false,doc.revision)[0]=="中文标题");
    store.set_starred(source.id,true);store.remove_feed(fid);assert(!store.translation_blocks(source.id,false,doc.revision).empty());
    store.set_starred(source.id,false);store.remove_feed(fid);assert(store.translation_blocks(source.id,false,doc.revision).empty());
    std::filesystem::remove_all(folder);
    std::cout<<"Qwen configuration, JSON, cancellation and translation cache checks passed\n";
}
