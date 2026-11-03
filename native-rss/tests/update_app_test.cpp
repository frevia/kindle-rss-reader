#include "app.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>

int main() {
    std::string font=std::getenv("RSS_FONT")?std::getenv("RSS_FONT"):"/System/Library/Fonts/STHeiti Light.ttc";
    if(!std::filesystem::exists(font))font="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    rss::Store store(":memory:");rss::Canvas canvas(1072,1448,font);rss::App app(store,canvas);
    const auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
    const auto shot=[&](const char* name){app.draw();if(const auto folder=std::getenv("RSS_TEST_SNAPSHOTS")) {
        std::filesystem::create_directories(folder);canvas.write_pgm((std::filesystem::path(folder)/name).string());
    }};
    tap(100,1392);tap(140,1070);assert(app.screen()==rss::App::Screen::Settings);shot("update-settings.pgm");
    tap(300,984);assert(app.screen()==rss::App::Screen::Update);shot("update-idle.pgm");
    tap(140,1380);assert(app.take_request()=="check-update"&&app.update_state()==rss::App::UpdateState::Checking);
    tap(140,1380);assert(app.take_request()=="cancel-update");app.update_finished(true);assert(app.update_state()==rss::App::UpdateState::Idle);
    rss::UpdateRelease release;release.version="v0.0.4";release.available=true;
    release.notes="升级说明\n\n增加在线升级，保留订阅与阅读位置。\n修复下载中断后的恢复。";
    auto long_notes=release;
    for(int i=0;i<30;++i)long_notes.notes+="\n更新项目 "+std::to_string(i+1);
    app.update_checked(long_notes);app.draw();const auto first_notes=canvas.pixels;shot("update-notes-first.pgm");
    tap(900,1190);app.draw();assert(canvas.pixels!=first_notes);shot("update-notes-next.pgm");
    tap(140,1190);app.draw();assert(canvas.pixels==first_notes&&app.take_request().empty());
    app.update_checked(release);shot("update-available.pgm");
    tap(900,1380);assert(app.take_request()=="download-update"&&app.update_state()==rss::App::UpdateState::Downloading);
    app.update_progress("已下载 12 MB");shot("update-downloading.pgm");
    app.back();assert(app.screen()==rss::App::Screen::Settings);
    app.back();assert(app.screen()==rss::App::Screen::List);
    app.set_reader_status({100,false,true,"12:00"});app.maybe_auto_sync(2000000000);assert(app.take_request().empty());
    tap(100,1392);tap(140,1070);tap(300,984);app.update_finished(false,"安装包 SHA256 校验失败");shot("update-failed.pgm");
    assert(!app.quit&&app.update_state()==rss::App::UpdateState::Failed);
    tap(900,1380);assert(app.take_request().empty());
    app.update_checked(release);app.update_started(true);app.update_finished(false);shot("update-ready.pgm");
    assert(app.update_state()==rss::App::UpdateState::Ready&&!app.quit);
    app.sync_started();tap(900,1380);assert(app.take_request().empty());app.sync_finished(true);
    tap(900,1380);assert(app.take_request()=="install-update"&&!app.quit);
    app.update_started(false);release.available=false;app.update_checked(release);assert(app.update_state()==rss::App::UpdateState::Current);
    tap(900,1380);assert(app.take_request().empty());shot("update-current.pgm");
}
