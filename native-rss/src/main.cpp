#include "app.hpp"
#include "feed.hpp"
#include "net.hpp"
#include "device.hpp"
#include "process.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <csignal>
#include <thread>
#include <unistd.h>
#ifdef RSS_DESKTOP
#include <SDL.h>
#endif

namespace {
std::string read_file(const std::string& path){
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("无法读取文件："+path);
    std::string text;char buffer[8192];
    while(in){in.read(buffer,sizeof(buffer));text.append(buffer,static_cast<std::size_t>(in.gcount()));
        if(text.size()>8*1024*1024)throw std::runtime_error("文件超过 8 MB 上限");}
    if(!in.eof())throw std::runtime_error("文件读取失败");return text;
}
std::string font_path(){
    if(const auto* env=std::getenv("RSS_FONT"))return env;
    for(const auto* path:{"/mnt/us/native-rss/fonts/reader.ttf","/usr/java/lib/fonts/HeiseiMaruGoth_JIS0213_E.ttf",
        "/System/Library/Fonts/STHeiti Light.ttc","/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"})
        if(std::filesystem::exists(path))return path;
    throw std::runtime_error("请通过 --font 指定中文字体");
}
std::string status_font_path(){
    const std::string path="/mnt/us/fonts/AlibabaPuHuiTi-3-55-Regular.ttf";
    std::error_code error;
    return std::filesystem::is_regular_file(path,error)?path:std::string{};
}
void demo(rss::Store& store){
    rss::Feed feed;feed.url="https://example.invalid/demo.xml";feed.title="阅读示例";feed.group="示例内容";
    const auto id=store.add_feed(feed);rss::ParsedFeed parsed;
    const std::vector<std::string> titles={"把注意力还给阅读：一份更安静的信息生活指南",
        "城市里的树，如何改变我们每天走过的街道", "从一杯咖啡开始，观察日常生活中的设计",
        "当我们慢下来，世界并没有停止向前", "独立博客为什么仍然值得订阅",
        "周末阅读清单：技术、人文，以及值得留白的时间", "写给未来的自己：保存那些值得重读的文字",
        "一个很长的中文文章标题应该完整展示重要信息，而不是为了整齐就让读者猜测被省略的那半句话"};
    for(std::size_t i=0;i<titles.size();++i){
        rss::Entry e;e.guid="demo-"+std::to_string(i);e.title=titles[i];e.url="https://example.invalid/article/"+std::to_string(i);
        e.published=std::time(nullptr)-static_cast<std::int64_t>(i)*3600;
        e.text="这是一篇用于检查中文排版的本地示例，并非来自真实订阅。\n\n";
        for(int j=0;j<12;++j)e.text+="阅读不必追赶每一条消息。把喜欢的作者放进订阅列表，给值得读的文字留出一点时间。\n\n"
            "你可以在这里调整字号、行距与页边距，也可以随时返回列表。阅读位置会保存在本地，下次打开时继续。"
            "离线的时候，已经保存的内容依然可用。界面不应抢走注意力，而应该让文字自然地留在眼前。\n\n";
        e.html="<p>本地排版示例</p>";parsed.entries.push_back(e);
    }
    store.save_entries(id,parsed);
}
void import_opml(rss::Store& store,const std::string& path){
    std::vector<std::string> groups;const auto feeds=rss::parse_opml(read_file(path),&groups);
    auto existing=store.groups();std::set<std::string> known(existing.begin(),existing.end());
    for(const auto& group:groups)if(!group.empty()&&!known.count(group)){
        store.add_group(group);known.insert(group);for(auto slash=group.find('/');slash!=std::string::npos;slash=group.find('/',slash+1))known.insert(group.substr(0,slash));
    }
    for(const auto& feed:feeds)store.add_feed(feed);
    std::cout<<"导入 "<<feeds.size()<<" 个订阅（重复地址自动合并）\n";
}
void write_export(rss::Store& store,const std::string& path){
    const auto temp=path+".tmp";std::ofstream out(temp,std::ios::binary);
    out<<rss::export_opml(store.feeds(),store.groups());out.close();if(!out)throw std::runtime_error("导出文件写入失败");
    std::filesystem::rename(temp,path);
}
void open_system_browser(const std::string& url,bool kindle){
    if(kindle){
        rss::run_bounded({"/usr/bin/lipc-set-prop","com.lab126.appmgrd","start","app://com.lab126.browser"},std::chrono::seconds(3));
        // The browser registers its LIPC service after appmgrd starts it.
        for(int attempt=0;attempt<5;++attempt){
            try{rss::run_bounded({"/usr/bin/lipc-set-prop","com.lab126.browser","gotoURL",url},std::chrono::seconds(1));return;}
            catch(const std::exception&){if(attempt==4)throw;std::this_thread::sleep_for(std::chrono::milliseconds(250));}
        }
    }else{
#ifdef __APPLE__
        rss::run_bounded({"/usr/bin/open",url},std::chrono::seconds(5));
#else
        rss::run_bounded({"/usr/bin/xdg-open",url},std::chrono::seconds(5));
#endif
    }
}
class UiTasks {
public:
    UiTasks(rss::Store& store,rss::App& app,const std::string& db):store_(store),app_(app),db_(db){
        // Sample before the first e-ink frame instead of refreshing an unknown status immediately after it.
        app_.set_reader_status(rss::read_reader_status(std::time(nullptr)));
        status_due_=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        if(db_=="/mnt/us/native-rss/data/rss.sqlite") {
            const std::filesystem::path result="/mnt/us/native-rss/.update/result";
            if(std::filesystem::is_regular_file(result)&&!std::filesystem::is_symlink(result)) {
                app_.notify(read_file(result.string()));std::filesystem::remove(result);
            }
        }
        const auto browser_error=store_.setting("directory_browser_error");
        if(!browser_error.empty()){app_.notify(browser_error);store_.set_setting("directory_browser_error","");}
    }
    ~UiTasks(){
        cancel_=true;check_cancel_=true;
        if(worker_.valid())worker_.wait();
        if(update_check_.valid())update_check_.wait();
    }
    void tick(){
        const auto now=std::chrono::steady_clock::now();
        if(update_task_==UpdateTask::Install&&now>=install_redraw_due_){app_.dirty=true;install_redraw_due_=now+std::chrono::seconds(2);}
        if(now>=status_due_){app_.set_reader_status(rss::read_reader_status(std::time(nullptr)));status_due_=now+std::chrono::seconds(30);}
        collect();auto request=app_.take_request();
        if(app_.quit)return;
        if(request=="prioritize-translation"){
            if(automatic_sync_task_&&worker_.valid())cancel_=true;
            request.clear();
        }
        if(request.empty()&&!worker_.valid())request=app_.take_queued_translation();
        if(request.empty()&&!worker_.valid()&&now>=auto_due_){
            auto_due_=now+std::chrono::seconds(30);
            try{app_.maybe_auto_sync(std::time(nullptr));request=app_.take_request();}
            catch(const std::exception& e){auto_due_=now+std::chrono::minutes(30);std::cerr<<"RSS Reader: automatic sync deferred: "<<e.what()<<'\n';}
        }
        const auto folder=std::filesystem::path(db_).parent_path();
        if(request.rfind("refresh-directory:",0)==0){
            if(worker_.valid()){app_.directory_finished(false,"请先等待当前后台任务完成");return;}
            try{
                const auto scope=request.substr(18);std::vector<std::string> ids;
                for(const auto& directory:rss::directory_catalogs(store_))if(directory.kind==rss::DirectoryKind::Opml&&(scope.empty()||directory.id==scope))ids.push_back(directory.id);
                if(ids.empty())throw std::runtime_error("没有可下载的 OPML 源库");
                cancel_=false;directory_task_=true;app_.directory_started();
                worker_=std::async(std::launch::async,[this,ids]{
                    rss::Store background(db_);std::string errors;
                    for(const auto& id:ids){
                        if(cancel_)break;
                        try{rss::refresh_directory(background,id,cancel_);}
                        catch(const std::exception& e){if(cancel_)break;if(!errors.empty())errors+="；";errors+=id+"："+e.what();}
                    }
                    if(!errors.empty())throw std::runtime_error(errors);
                });
            }catch(const std::exception& e){directory_task_=false;app_.directory_finished(false,e.what());}
        }else if(request=="cancel-directory"){
            if(directory_task_)cancel_=true;
        }else if(request=="check-update") {
            if(update_check_.valid())return;
            if(update_task_!=UpdateTask::None){app_.update_finished(false,"请先等待当前更新操作完成");return;}
            try {
                check_cancel_=false;app_.update_started(false);
                update_check_=std::async(std::launch::async,[this] {
                    const auto progress=[this](const std::string& message){
                        std::lock_guard<std::mutex> lock(mutex_);check_progress_=message;
                    };
                    return rss::check_update(check_cancel_,progress);
                });
            }catch(const std::exception& e){app_.update_finished(false,e.what());}
        }else if(request=="download-update") {
            if(worker_.valid()||update_check_.valid()){app_.update_finished(false,"请先等待当前后台任务完成");return;}
            try {
                require_device_update();
                cancel_=false;update_task_=UpdateTask::Download;app_.update_started(true);
                worker_=std::async(std::launch::async,[this] {
                    const auto progress=[this](const std::string& message){
                        std::lock_guard<std::mutex> lock(mutex_);progress_=rss::SyncProgress{0,0,0,message};
                    };
                    rss::prepare_update(update_result_,"/mnt/us",cancel_,progress);
                });
            }catch(const std::exception& e){update_task_=UpdateTask::None;app_.update_finished(false,e.what());}
        }else if(request=="cancel-update") {
            if(update_check_.valid()){check_cancel_=true;app_.update_progress("正在取消检查…");}
            else if(update_task_!=UpdateTask::None){cancel_=true;app_.update_progress("正在取消更新…");}
        }else if(request=="install-update") {
            try {
                require_device_update();
                if(worker_.valid()||update_check_.valid())throw std::runtime_error("请先等待当前后台任务完成");
                if(std::filesystem::exists("/var/local/native-rss/home/enabled"))throw std::runtime_error("请先在 KUAL 停用首页 RSS");
                cancel_=false;update_task_=UpdateTask::Install;
                install_redraw_due_=now+std::chrono::seconds(2);
                worker_=std::async(std::launch::async,[this]{
                    rss::backup_update_database("/mnt/us",[this](const std::string& message){
                        std::lock_guard<std::mutex> lock(mutex_);progress_=rss::SyncProgress{0,0,0,message};
                    });
                });
            }catch(const std::exception& e){update_task_=UpdateTask::None;app_.update_finished(false,e.what());}
        }else if(request=="sync"||request=="retry-sync"||request=="auto-sync"||request=="translate-selection"||request.rfind("fulltext:",0)==0||request.rfind("translate:",0)==0){
            if(update_task_!=UpdateTask::None||directory_task_) {
                if(const auto selection=app_.take_selection_request())app_.selection_finished(*selection,"",false,"请先等待在线升级任务完成");
                app_.sync_finished(false,"请先等待源库下载或在线升级任务完成");return;
            }
            if(!worker_.valid()){
                const bool selecting=request=="translate-selection",article_translation=request.rfind("translate:",0)==0;
                const bool translating=selecting||article_translation;
                const bool automatic=request=="auto-sync";
                cancel_=false;
                try{
                    selection_result_.clear();selection_job_=selecting?app_.take_selection_request():std::nullopt;
                    if(selecting&&!selection_job_)throw std::runtime_error("请重新选择需要翻译的文字");
                    if(!automatic&&(request=="sync"||request=="retry-sync"))store_.set_setting("auto_sync_last_attempt",std::to_string(std::time(nullptr)));
                    app_.sync_started(translating,automatic);const bool failed_only=request=="retry-sync";
                    const auto fulltext_id=request.rfind("fulltext:",0)==0?std::stoll(request.substr(9)):0;
                    const auto translation_id=article_translation?std::stoll(request.substr(14)):0;
                    const bool translation_web=article_translation&&request.substr(10,4)=="web:";
                    const auto selection=selection_job_;
                    if(selection)app_.sync_progress(0,1,0,"正在翻译选中文字");
                    worker_=std::async(std::launch::async,[this,failed_only,fulltext_id,translation_id,translation_web,selection]{
                        rss::Store background(db_);const auto progress=[this](const rss::SyncProgress& p){
                            std::lock_guard<std::mutex> lock(mutex_);progress_=p;
                        };
                        if(selection)selection_result_=rss::translate_selection(background,*selection,rss::qwen_config(db_),cancel_);
                        else if(translation_id)rss::translate_article(background,translation_id,translation_web,rss::qwen_config(db_),cancel_,progress);
                        else if(fulltext_id)rss::sync_fulltext(background,fulltext_id,cancel_,progress);
                        else rss::sync_feeds(background,cancel_,progress,failed_only);
                    });
                    automatic_sync_task_=automatic;
                }catch(const std::exception& e){
                    if(selection_job_){app_.selection_finished(*selection_job_,"",false,e.what());selection_job_.reset();}
                    else app_.sync_finished(false,e.what());
                    if(!automatic&&!selecting)throw;
                }
            }
        }else if(request=="cancel-sync"){
            if(directory_task_||update_task_!=UpdateTask::None){app_.sync_finished(true);return;}
            cancel_=true;if(!worker_.valid())app_.sync_finished(true);else app_.notify("正在取消同步…");
        }else if(request=="import"){
            import_opml(store_,(folder/"feeds.opml").string());app_.reload();app_.notify(store_.setting("auto_sync")=="0"?"订阅已导入，点击同步获取文章":"订阅已导入，将按自动同步周期更新，也可手动同步");
        }else if(request=="export"){
            write_export(store_,(folder/"subscriptions.opml").string());app_.notify("已导出 subscriptions.opml");
        }
        collect();
    }
private:
    void require_device_update() const {
        const auto guard=std::getenv("RSS_UI_GUARD");
        if(!guard||std::string(guard)!="1"||db_!="/mnt/us/native-rss/data/rss.sqlite")
            throw std::runtime_error("下载安装仅支持从 Kindle 设备启动器运行");
    }
    void progress(const rss::SyncProgress& value) {
        if(update_task_!=UpdateTask::None)app_.update_progress(value.message);
        else app_.sync_progress(value.completed,value.total,value.failed,value.message);
    }
    void collect_update_check(){
        if(!update_check_.valid())return;
        if(update_check_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {
                auto release=update_check_.get();
                if(check_cancel_)app_.update_finished(true);
                else {
                    update_result_=std::move(release);
                    app_.update_checked(update_result_);
                }
            }catch(const std::exception& e){app_.update_finished(check_cancel_,e.what());}
            // The worker has exited, so no progress from this check can reach the next one.
            std::lock_guard<std::mutex> lock(mutex_);
            check_progress_.reset();
        }else if(!check_cancel_) {
            std::optional<std::string> message;
            {std::lock_guard<std::mutex> lock(mutex_);message=std::move(check_progress_);check_progress_.reset();}
            if(message)app_.update_progress(*message);
        }
    }
    void collect(){
        collect_update_check();
        std::optional<rss::SyncProgress> progress;
        {std::lock_guard<std::mutex> lock(mutex_);progress=std::move(progress_);progress_.reset();}
        if(progress)this->progress(*progress);
        // Paint the preparation status before allowing even a fast backup to close the UI.
        if(update_task_==UpdateTask::Install&&app_.dirty)return;
        if(worker_.valid()&&worker_.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            std::string failure;try{worker_.get();}catch(const std::exception& e){failure=e.what();}
            automatic_sync_task_=false;
            // Worker state is committed even when its status page is not visible.
            {std::lock_guard<std::mutex> lock(mutex_);progress=std::move(progress_);progress_.reset();}
            if(progress)this->progress(*progress);
            if(directory_task_){directory_task_=false;app_.directory_finished(cancel_,failure);}
            else if(update_task_!=UpdateTask::None) {
                if(update_task_==UpdateTask::Install&&failure.empty()&&!cancel_){app_.install_update=true;app_.quit=true;}
                else app_.update_finished(cancel_,failure);
                update_task_=UpdateTask::None;
            }else if(selection_job_){app_.selection_finished(*selection_job_,selection_result_,cancel_,failure);selection_job_.reset();selection_result_.clear();}
            else app_.sync_finished(cancel_,failure);
        }
    }
    rss::Store& store_;rss::App& app_;std::string db_;
    std::chrono::steady_clock::time_point status_due_{},auto_due_{},install_redraw_due_{};
    std::optional<rss::SelectionJob> selection_job_;
    enum class UpdateTask { None, Download, Install };
    UpdateTask update_task_=UpdateTask::None;
    bool directory_task_=false;
    bool automatic_sync_task_=false;
    rss::UpdateRelease update_result_;
    std::string selection_result_;
    std::atomic<bool> cancel_{false},check_cancel_{false};
    std::future<void> worker_;
    std::future<rss::UpdateRelease> update_check_;
    std::mutex mutex_;
    std::optional<rss::SyncProgress> progress_;
    std::optional<std::string> check_progress_;
};
volatile std::sig_atomic_t stop_device=0;
void stop_device_handler(int){stop_device=1;}
void install_update(const std::filesystem::path& root,const std::function<void(const std::string&,int)>& present){
    const auto started=std::chrono::steady_clock::now();
    std::mutex mutex;std::string message="正在校验安装文件";
    present(message,0);
    auto worker=std::async(std::launch::async,[&]{
        rss::apply_update(root,[&](const std::string& value){std::lock_guard<std::mutex> lock(mutex);message=value;});
    });
    auto redraw=started+std::chrono::seconds(2);
    while(worker.wait_for(std::chrono::milliseconds(100))!=std::future_status::ready){
        const auto now=std::chrono::steady_clock::now();
        if(now<redraw)continue;
        std::string current;{std::lock_guard<std::mutex> lock(mutex);current=message;}
        present(current,static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now-started).count()));
        redraw=now+std::chrono::seconds(2);
    }
    const auto elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started).count());
    try{worker.get();}
    catch(...){present("安装中止，正在恢复原版本，请稍候",elapsed);throw;}
    present("安装完成，正在重新打开阅读器",elapsed);
}
int device(rss::Store& store,rss::Canvas& canvas,const std::string& db,std::int64_t article){
    const auto guard=std::getenv("RSS_UI_GUARD");
    if(!guard||std::string(guard)!="1")throw std::runtime_error("设备模式必须由屏幕会话启动器运行");
    stop_device=0;
    const auto term=std::signal(SIGTERM,stop_device_handler),interrupt=std::signal(SIGINT,stop_device_handler),hangup=std::signal(SIGHUP,stop_device_handler);
    bool install=false,browser=false,activate_home=false;
    try{
        rss::KindleDevice hardware;rss::App app(store,canvas,article,status_font_path());
        if(const auto* state=std::getenv("RSS_HOME_AVAILABILITY")) {
            const std::string_view availability(state);
            if(availability=="active")app.set_home_availability(rss::App::HomeAvailability::Active);
            else if(availability=="available")app.set_home_availability(rss::App::HomeAvailability::Available);
            else if(availability=="unavailable")app.set_home_availability(rss::App::HomeAvailability::Unavailable);
        }
        UiTasks tasks(store,app,db);
        auto last_activity=std::chrono::steady_clock::now()-std::chrono::seconds(5);
        while(!stop_device&&!app.quit){
            if(app.dirty){
                app.draw();hardware.present(canvas.pixels);
                if(std::filesystem::exists("/mnt/us/native-rss/.update/journal")) {
                    rss::commit_update("/mnt/us");app.notify("在线升级完成 · "+rss::app_version());
                }
            }
            const auto gesture=hardware.touch(100);
            const auto now=std::chrono::steady_clock::now();
            if(gesture&&now-last_activity>=std::chrono::seconds(5)){
                rss::run_bounded({"/usr/bin/lipc-set-prop","-i","com.lab126.powerd","touchScreenSaverTimeout","1"},std::chrono::seconds(2));
                last_activity=now;
            }
            if(stop_device)break;
            try{if(gesture)app.touch(*gesture);if(!stop_device)tasks.tick();}
            catch(const std::exception& e){app.show_error(e.what());}
        }
        // The supervising launcher restores the screen after checking the power state.
        install=app.install_update;
        browser=!app.open_website.empty();
        activate_home=!stop_device&&app.quit&&app.home_activation_requested();
    }catch(...){std::signal(SIGTERM,term);std::signal(SIGINT,interrupt);std::signal(SIGHUP,hangup);throw;}
    std::signal(SIGTERM,term);std::signal(SIGINT,interrupt);std::signal(SIGHUP,hangup);
    if(install)return 42;
    if(browser)return 43;
    return activate_home?44:0;
}
#ifdef RSS_DESKTOP
void desktop(rss::Store& store,rss::Canvas& canvas,const std::string& db,std::int64_t article){
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0)throw std::runtime_error(SDL_GetError());
    auto window=SDL_CreateWindow("Kindle RSS · 原生桌面模拟",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        canvas.width/2,canvas.height/2,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!window){SDL_Quit();throw std::runtime_error(SDL_GetError());}
    auto renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    auto texture=renderer?SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,canvas.width,canvas.height):nullptr;
    if(!texture){if(renderer)SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();throw std::runtime_error("无法创建模拟窗口");}
    SDL_RenderSetLogicalSize(renderer,canvas.width,canvas.height);SDL_StartTextInput();
    rss::App app(store,canvas,article,status_font_path());std::vector<std::uint32_t> rgba(canvas.pixels.size());
    UiTasks tasks(store,app,db);
    rss::TouchDecoder pointer;
    bool pointer_down=false;
    const auto touch_time=[](){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    const auto input=[&](std::uint16_t type,std::uint16_t code,std::int32_t value){if(auto gesture=pointer.consume(type,code,value,touch_time()))app.touch(*gesture);};
    try {
        while(!app.quit){
            SDL_Event event;
            if(SDL_WaitEventTimeout(&event,80)){
                try {
                    if(event.type==SDL_QUIT)app.quit=true;
                    else if(event.type==SDL_MOUSEBUTTONDOWN&&event.button.button==SDL_BUTTON_LEFT){
                        pointer_down=true;input(3,47,0);input(3,57,1);input(3,53,event.button.x);input(3,54,event.button.y);input(0,0,0);
                    }
                    else if(event.type==SDL_MOUSEMOTION&&pointer_down){
                        input(3,53,event.motion.x);input(3,54,event.motion.y);input(0,0,0);
                    }
                    else if(event.type==SDL_MOUSEBUTTONUP&&event.button.button==SDL_BUTTON_LEFT){
                        if(pointer_down){input(3,53,event.button.x);input(3,54,event.button.y);input(3,57,-1);input(0,0,0);pointer_down=false;}
                    }
                    else if(event.type==SDL_TEXTINPUT)app.text_input(event.text.text);
                    else if(event.type==SDL_KEYDOWN){
                        const auto key=event.key.keysym.sym;
                        if((event.key.keysym.mod&(KMOD_CTRL|KMOD_GUI))&&key==SDLK_v){auto text=SDL_GetClipboardText();if(text){app.text_input(text);SDL_free(text);}}
                        else if(key==SDLK_ESCAPE)app.back();
                        else if(key==SDLK_RIGHT||key==SDLK_PAGEDOWN)app.next();
                        else if(key==SDLK_LEFT||key==SDLK_PAGEUP)app.previous();
                        else if(key==SDLK_BACKSPACE)app.backspace();
                        else if(key==SDLK_RETURN)app.submit();
                    }else if(event.type==SDL_WINDOWEVENT){
                        if(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST||event.window.event==SDL_WINDOWEVENT_LEAVE||
                           event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED){
                            if(pointer_down){rss::TouchGesture cancel({0,0});cancel.phase=rss::TouchPhase::Cancel;app.touch(cancel);}
                            pointer=rss::TouchDecoder{};pointer_down=false;
                        }
                        app.dirty=true;
                    }
                }catch(const std::exception& e){app.show_error(e.what());}
            }
            try{if(auto gesture=pointer.poll(touch_time()))app.touch(*gesture);tasks.tick();}catch(const std::exception& e){app.show_error(e.what());}
            if(app.dirty){
                app.draw();for(std::size_t i=0;i<rgba.size();++i){const auto g=canvas.pixels[i];rgba[i]=0xff000000u|(g<<16)|(g<<8)|g;}
                SDL_UpdateTexture(texture,nullptr,rgba.data(),canvas.width*4);
                SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);SDL_RenderPresent(renderer);
            }
        }
    }catch(...){SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();throw;}
    SDL_StopTextInput();SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
    if(!app.open_website.empty()){
        store.set_setting("directory_browser_url","");open_system_browser(app.open_website,false);
    }
}
#endif
}
int main(int argc,char** argv){
    try {
        umask(0077);
        std::string db="rss.sqlite",font,command="help",arg;std::int64_t article=0;
        for(int i=1;i<argc;++i){
            std::string token=argv[i];
            if(token=="--db"||token=="--font"||token=="--article"){
                if(++i==argc)throw std::runtime_error("缺少参数："+token);
                if(token=="--db")db=argv[i];else if(token=="--font")font=argv[i];else {
                    const std::string value=argv[i];std::size_t used=0;
                    article=std::stoll(value,&used);
                    if(value.empty()||value[0]<'1'||value[0]>'9'||used!=value.size()||article<=0)
                        throw std::runtime_error("需要有效的文章编号");
                }
            }else if(command=="help")command=token;
            else if(arg.empty())arg=token;
            else throw std::runtime_error("多余参数："+token);
        }
        if(command=="help"||command=="--help"){
            std::cout<<"Kindle RSS 原生阅读器\n"
                "用法：kindle-rss [--db FILE] [--font FONT] COMMAND [ARG]\n"
                "  demo                 写入明确标注的排版示例\n"
                "  import FILE.opml     导入并合并订阅\n"
                "  export FILE.opml     导出订阅\n"
                "  add URL              添加 RSS/Atom 订阅\n"
                "  sync [failed]        更新订阅，或重试失败项目\n"
                "  fulltext ID          下载单篇网页正文并缓存图片\n"
                "  translate ID         英译中并缓存 RSS 内容\n"
                "  translate-web ID     英译中并缓存已下载的网页正文\n"
                "  list                 列出本地文章\n"
                "  probe                只读检查 Kindle 屏幕和输入设备\n"
                "  check-update         只读检查 GitHub 当前通道新版本\n"
                "  device               由会话启动器启动 Kindle 原生界面；--article ID 直达正文\n"
                "  render FILE.pgm      渲染 1072×1448 列表；--article ID 渲染正文\n"
                "  desktop              启动原生桌面模拟器\n";return 0;
        }
        if(command=="probe"){std::cout<<rss::probe_device();return 0;}
        if(command=="check-update") {
            const std::atomic<bool> cancel{false};const auto update=rss::check_update(cancel);
            std::cout<<"当前 "<<rss::app_version()<<" · 最新 "<<update.version<<(update.available?" · 有可用更新\n":" · 已是最新版本\n");return 0;
        }
        if(command=="update-apply"||command=="update-recover") {
            if(!arg.empty())throw std::runtime_error("升级命令不接受额外参数");
            const auto guard=std::getenv("RSS_UPDATE_GUARD");
            if(!guard||std::string(guard)!="1")throw std::runtime_error("升级命令必须由设备启动器运行");
            if(command=="update-apply") {
                const auto launcher=std::getenv("RSS_LAUNCHER_PID");rss::bind_to_launcher(launcher?launcher:"");
                try{
                    rss::KindleDevice hardware;rss::Canvas canvas(1072,1448,font_path());
                    install_update("/mnt/us",[&](const std::string& message,int elapsed){
                        rss::draw_update_installation(canvas,message,elapsed);hardware.present(canvas.pixels);
                    });
                    return 0;
                }
                catch(const std::exception& e) {
                    rss::recover_update("/mnt/us");
                    std::ofstream result("/mnt/us/native-rss/.update/result");result<<"在线升级未完成，已保留原版本："<<e.what();
                    std::cerr<<"升级失败："<<e.what()<<'\n';return 1;
                }
            }
            rss::recover_update("/mnt/us");::unsetenv("RSS_UPDATE_BOOT");
            ::close(9);
            ::execl("/bin/sh","sh","/mnt/us/native-rss/launch.sh",static_cast<char*>(nullptr));
            throw std::runtime_error("无法重新启动阅读器，请从书库再次打开");
        }
        if(command=="device"){
            const auto launcher=std::getenv("RSS_LAUNCHER_PID");
            rss::bind_to_launcher(launcher?launcher:"");
        }
        const auto parent=std::filesystem::path(db).parent_path();if(!parent.empty())std::filesystem::create_directories(parent);
        rss::Store store(db);
        if(command=="open-directory-website"){
            const auto guard=std::getenv("RSS_BROWSER_GUARD");
            if(!guard||std::string(guard)!="1"||!arg.empty())throw std::runtime_error("浏览器交接必须在设备启动器恢复系统界面后运行");
            const auto url=store.setting("directory_browser_url");const auto directories=rss::directory_catalogs(store);
            if(url.empty()||std::none_of(directories.begin(),directories.end(),[&](const rss::Directory& d){return d.kind==rss::DirectoryKind::Website&&d.url==url;}))throw std::runtime_error("网站入口已移除或地址无效");
            store.set_setting("directory_browser_url","");
            try{open_system_browser(url,true);}
            catch(const std::exception&){store.set_setting("directory_browser_error","系统浏览器未能打开网站，请在源库页查看地址。");throw;}
        }
        else if(command=="demo")demo(store);
        else if(command=="import"){if(arg.empty())throw std::runtime_error("需要 OPML 路径");import_opml(store,arg);}
        else if(command=="export"){if(arg.empty())throw std::runtime_error("需要导出路径");write_export(store,arg);}
        else if(command=="add"){
            if(arg.rfind("http://",0)!=0&&arg.rfind("https://",0)!=0)throw std::runtime_error("需要 HTTP(S) 地址");
            rss::Feed feed;feed.url=arg;feed.title=arg;store.add_feed(feed);
        }else if(command=="sync"){
            if(!arg.empty()&&arg!="failed")throw std::runtime_error("同步参数仅支持 failed");
            std::atomic<bool> cancel{false};int failed=0;
            rss::sync_feeds(store,cancel,[&](const rss::SyncProgress& p){failed=p.failed;std::cout<<p.completed<<"/"<<p.total<<" "<<p.message<<"\n";},arg=="failed");
            return failed?2:0;
        }else if(command=="fulltext"){
            std::size_t used=0;const auto id=std::stoll(arg,&used);
            if(id<=0||used!=arg.size())throw std::runtime_error("需要有效的文章编号");
            store.queue_fulltext(id);
            if(store.fulltext_status(id).state=="ready"){std::cout<<"网页正文已保存\n";return 0;}
            if(store.fulltext_status(id).state!="pending")throw std::runtime_error("文章地址不可下载，或订阅已取消");
            std::atomic<bool> cancel{false};int failed=0;
            rss::sync_fulltext(store,id,cancel,[&](const rss::SyncProgress& p){failed=p.failed;std::cout<<p.message<<"\n";});
            return failed?2:0;
        }else if(command=="translate"||command=="translate-web"){
            std::size_t used=0;const auto id=std::stoll(arg,&used);
            if(id<=0||used!=arg.size())throw std::runtime_error("需要有效的文章编号");
            std::atomic<bool> cancel{false};int failed=0;
            rss::translate_article(store,id,command=="translate-web",rss::qwen_config(db),cancel,[&](const rss::SyncProgress& p){failed=p.failed;std::cout<<p.message<<"\n";});
            return failed?2:0;
        }else if(command=="list"){
            for(const auto& e:store.entries(rss::View::All))std::cout<<e.id<<"\t"<<(e.read?"已读":"未读")<<"\t"<<e.title<<"\n";
        }else if(command=="render"||command=="desktop"||command=="device"){
            rss::Canvas canvas(1072,1448,font.empty()?font_path():font);
            if(command=="device")return device(store,canvas,db,article);
            else if(command=="render"){
                if(arg.empty())throw std::runtime_error("需要输出 PGM 路径");
                rss::App app(store,canvas,0,status_font_path());if(article)app.open(article,false);app.draw();canvas.write_pgm(arg);
            }else{
#ifdef RSS_DESKTOP
                desktop(store,canvas,db,article);
#else
                throw std::runtime_error("当前构建未启用桌面模拟器");
#endif
            }
        }else throw std::runtime_error("未知命令："+command);
        return 0;
    }catch(const std::exception& e){std::cerr<<"错误："<<e.what()<<"\n";return 1;}
}
