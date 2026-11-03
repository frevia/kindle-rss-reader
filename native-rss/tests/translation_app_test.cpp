#include "app.hpp"
#include "feed.hpp"
#include "translation.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    using namespace rss;
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    Canvas canvas(1072,1448,font);
    for(const auto* engine:{"classic","cre"}) {
        if(std::string(engine)=="cre"&&!CreReader::available())continue;
        Store store(":memory:");store.set_setting("reader_engine",engine);
        Feed feed;feed.url="https://example.invalid/feed";feed.title="双语阅读测试";const auto fid=store.add_feed(feed);
        Entry source;source.guid="one";source.title="A quiet reader";source.url="https://example.invalid/story";
        for(int i=0;i<80;++i)source.html+="<p>Paragraph "+std::to_string(i)+". Good tools help us focus on reading and keep our place when switching between languages.</p>";
        source.text=html_to_text(source.html);ParsedFeed parsed;parsed.entries={source};store.save_entries(fid,parsed);
        source=*store.entry(store.entries(View::All).front().id);const auto id=source.id;
        const auto document=translation_document(source);
        App app(store,canvas);app.open(id);app.next();app.next();assert(app.reader_page()>0);
        const auto original_page=app.reader_page();const auto original_position=store.entry(id)->position;
        const auto original_anchor=store.setting("reader_cre:"+std::to_string(id));
        const auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
        tap(536,600);tap(400,1380);tap(850,920);assert(app.reader_panel()==App::ReaderPanel::Translation);
        app.draw();const auto unconfigured=canvas.pixels;
        tap(180,1296);assert(app.take_request()=="translate:rss:"+std::to_string(id));
        app.sync_finished(false,"请先在 translation.conf 中设置 RSS_QWEN_API_KEY");app.draw();
        assert(app.sync_state()==App::SyncState::Failed&&app.reading_mode()==ReadingMode::Original);
        assert(canvas.pixels!=unconfigured);
        if(const auto path=std::getenv("RSS_TEST_SNAPSHOTS")) {
            std::filesystem::create_directories(path);canvas.write_pgm((std::filesystem::path(path)/(std::string("translation-config-error-")+engine+".pgm")).string());
        }
        tap(180,1296);assert(app.take_request()=="translate:rss:"+std::to_string(id));
        tap(180,1296);assert(app.take_request()=="cancel-sync");app.sync_finished(true);
        store.begin_translation(id,false,document.revision,document.blocks.size());
        for(std::size_t i=0;i<document.blocks.size();++i)store.save_translation_block(id,false,document.revision,i,i==0?"安静的阅读器":"第 "+std::to_string(i-1)+" 段：好的工具帮助我们专注阅读，并在切换语言时保留阅读位置。");
        store.set_translation_state(id,false,document.revision,"ready");
        app.sync_started(true);app.sync_progress(81,81,0,"翻译已保存");app.sync_finished(false);
        assert(app.reading_mode()==ReadingMode::Original&&app.reader_page()==original_page);
        tap(850,860);assert(app.reading_mode()==ReadingMode::Bilingual&&app.reader_page()>0);
        assert(store.entry(id)->position==original_position&&store.setting("reader_cre:"+std::to_string(id))==original_anchor);
        if(const auto path=std::getenv("RSS_TEST_SNAPSHOTS")) {
            std::filesystem::create_directories(path);app.draw();canvas.write_pgm((std::filesystem::path(path)/(std::string("translation-panel-")+engine+".pgm")).string());
            app.back();app.back();app.draw();canvas.write_pgm((std::filesystem::path(path)/(std::string("bilingual-reading-")+engine+".pgm")).string());
            tap(536,600);tap(400,1380);tap(850,920);
        }
        tap(530,860);assert(app.reading_mode()==ReadingMode::Translated);
        tap(170,860);assert(app.reading_mode()==ReadingMode::Original&&app.reader_page()>0);
        tap(850,860);assert(app.reading_mode()==ReadingMode::Bilingual);
        app.back();app.back();app.next();const auto bilingual_page=app.reader_page();
        app.back();assert(!app.reading());
        App reopened(store,canvas);reopened.open(id);assert(reopened.reading_mode()==ReadingMode::Bilingual&&reopened.reader_page()==bilingual_page);
        source.html+="<p>New paragraph.</p>";source.text=html_to_text(source.html);parsed.entries={source};store.save_entries(fid,parsed);
        reopened.open(id);assert(reopened.reading_mode()==ReadingMode::Original);
        assert(store.translation_status(id,false,translation_revision(*store.entry(id))).state=="stale");

        for(bool web:{false,true})for(const auto mode:{ReadingMode::Translated,ReadingMode::Bilingual}) {
            auto growing=source;growing.guid=std::string(web?"web-":"rss-")+(mode==ReadingMode::Bilingual?"dual":"translated");
            parsed.entries={growing};store.save_entries(fid,parsed);const auto growing_id=store.entries(View::All).front().id;
            if(web){store.queue_fulltext(growing_id);store.save_fulltext(growing_id,growing);}
            growing=*(web?store.fulltext_entry(growing_id):store.entry(growing_id));
            const auto growing_doc=translation_document(growing);
            store.begin_translation(growing_id,web,growing_doc.revision,growing_doc.blocks.size());
            store.save_translation_block(growing_id,web,growing_doc.revision,0,"逐步补全的译文");
            store.set_setting("reader_content:"+std::to_string(growing_id),web?"web":"rss");
            store.set_setting(std::string("reader_mode:")+(web?"web:":"rss:")+std::to_string(growing_id),mode==ReadingMode::Bilingual?"bilingual":"translated");
            App reading_partial(store,canvas);reading_partial.open(growing_id);
            for(int i=0;i<6;++i)reading_partial.next();assert(reading_partial.reader_page()>0);
            for(std::size_t i=1;i<growing_doc.blocks.size();++i)
                store.save_translation_block(growing_id,web,growing_doc.revision,i,"好的工具保留我们的阅读位置。后台补全译文后，再次打开仍应回到同一原文段落附近。");
            store.set_translation_state(growing_id,web,growing_doc.revision,"ready");
            reading_partial.back();
            const auto anchor_key=std::string("reader_source_anchor:")+(mode==ReadingMode::Bilingual?"bilingual:":"translated:")+(web?"web:":"")+std::to_string(growing_id);
            const auto saved_anchor=store.setting(anchor_key);assert(saved_anchor.find('|')!=std::string::npos);
            store.set_read(growing_id,false);
            App preview(store,canvas);preview.open(growing_id,false);
            assert(!store.entry(growing_id)->read&&store.setting(anchor_key)==saved_anchor&&preview.reader_page()>0);
            App completed(store,canvas);completed.open(growing_id);
            assert(completed.reading_mode()==mode&&completed.reader_page()>0);
            const auto restored_page=completed.reader_page();completed.back();
            const auto restored_anchor=store.setting(anchor_key);
            assert(restored_anchor.substr(restored_anchor.find('|'))==saved_anchor.substr(saved_anchor.find('|')));
            App stable(store,canvas);stable.open(growing_id);assert(stable.reader_page()==restored_page);
            if(!web) {
                auto updated=*store.entry(growing_id);updated.html="<p>This is a new first paragraph.</p>"+updated.html;updated.text=html_to_text(updated.html);
                parsed.entries={updated};store.save_entries(fid,parsed);
                const auto updated_doc=translation_document(*store.entry(growing_id));
                store.begin_translation(growing_id,false,updated_doc.revision,updated_doc.blocks.size());
                for(std::size_t i=0;i<updated_doc.blocks.size();++i)store.save_translation_block(growing_id,false,updated_doc.revision,i,"新原文的新译文。");
                store.set_translation_state(growing_id,false,updated_doc.revision,"ready");
                App changed(store,canvas);changed.open(growing_id);assert(changed.reader_page()==0);
            }
        }
    }
    std::cout<<"Original, translated and bilingual UI, positions and stale caches passed\n";
}
