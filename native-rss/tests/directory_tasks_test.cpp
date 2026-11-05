// Exercise the actual UI task coordinator without starting SDL or device services.
#define main rss_command_main
#include "../src/main.cpp"
#undef main
#include <cassert>

int main(int argc,char** argv){
    assert(argc==2);const std::string base=argv[1];
    const auto path=std::filesystem::temp_directory_path()/("rss-directory-tasks-"+std::to_string(getpid())+".sqlite");
    std::filesystem::remove(path);
    {
        rss::Store store(path.string());store.set_setting("reader_engine","classic");store.set_setting("auto_sync","0");
        rss::Directory directory;directory.title="Slow local catalog";directory.url=base+"/slow.opml";
        const auto id=rss::save_directory(store,directory);
        rss::Canvas canvas(1072,1448,font_path());rss::App app(store,canvas);UiTasks tasks(store,app,path.string());
        const auto tap=[&](int x,int y){app.draw();app.tap(x,y);};
        const auto wait=[&]{
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(app.directory_running()&&std::chrono::steady_clock::now()<until){tasks.tick();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            assert(!app.directory_running());
        };
        tap(100,1392);tap(140,966);tap(200,536);tap(200,364);
        assert(app.screen()==rss::App::Screen::DirectoryScope);tap(200,784);
        assert(app.screen()==rss::App::Screen::Discovery);
        tap(950,136);assert(app.take_request()=="refresh-directory:"+id);app.directory_finished(true);
        tap(950,136);tasks.tick();assert(app.directory_running());wait();
        assert(rss::directory_cache(store,id).feeds.size()==12&&store.feeds().empty());
        // A manual RSS sync cannot steal the directory worker or its completion callback.
        tap(950,136);tasks.tick();assert(app.directory_running());app.back();app.back();
        tap(100,1392);tap(140,1174);tasks.tick();
        assert(app.directory_running()&&app.sync_state()==rss::App::SyncState::Failed);
        wait();assert(rss::directory_cache(store,id).feeds.size()==12);
        app.back();tap(100,1392);tap(140,966);tap(200,536);
        // Cancelling the task preserves the last successful cache.
        tap(950,136);tasks.tick();assert(app.directory_running());tap(950,136);tasks.tick();wait();
        assert(rss::directory_cache(store,id).feeds.size()==12&&store.feeds().empty());
        // Rejected refresh attempts leave the current sync untouched.
        app.sync_started();bool rejected=false;try{tap(950,136);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected&&!app.directory_running()&&app.sync_state()==rss::App::SyncState::Running);app.sync_finished(true);
    }
    std::filesystem::remove(path);std::filesystem::remove(path.string()+"-wal");std::filesystem::remove(path.string()+"-shm");
}
