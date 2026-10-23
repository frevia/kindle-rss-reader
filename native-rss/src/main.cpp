#include "app.hpp"
#include "feed.hpp"
#include "net.hpp"
#include "device.hpp"
#include "process.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <csignal>
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
    const auto feeds=rss::parse_opml(read_file(path));for(const auto& feed:feeds)store.add_feed(feed);
    std::cout<<"导入 "<<feeds.size()<<" 个订阅（重复地址自动合并）\n";
}
void write_export(rss::Store& store,const std::string& path){
    const auto temp=path+".tmp";std::ofstream out(temp,std::ios::binary);
    out<<rss::export_opml(store.feeds());out.close();if(!out)throw std::runtime_error("导出文件写入失败");
    std::filesystem::rename(temp,path);
}
class UiTasks {
public:
    UiTasks(rss::Store& store,rss::App& app,const std::string& db):store_(store),app_(app),db_(db){}
    ~UiTasks(){cancel_=true;if(worker_.valid())worker_.wait();}
    void tick(){
        const auto request=app_.take_request();
        const auto folder=std::filesystem::path(db_).parent_path();
        if(request=="sync"){
            if(worker_.valid()){cancel_=true;app_.notify("正在取消同步…");}
            else{
                cancel_=false;app_.notify("正在同步…");
                worker_=std::async(std::launch::async,[this]{
                    rss::Store background(db_);
                    rss::sync_feeds(background,cancel_,[this](const rss::SyncProgress& p){
                        std::lock_guard<std::mutex> lock(mutex_);progress_=p.message;
                    });
                });
            }
        }else if(request=="import"){
            import_opml(store_,(folder/"feeds.opml").string());app_.reload();app_.notify("订阅已导入，点击同步获取文章");
        }else if(request=="export"){
            write_export(store_,(folder/"subscriptions.opml").string());app_.notify("已导出 subscriptions.opml");
        }
        {std::lock_guard<std::mutex> lock(mutex_);if(!progress_.empty()){app_.notify(progress_);progress_.clear();}}
        if(worker_.valid()&&worker_.wait_for(std::chrono::seconds(0))==std::future_status::ready)worker_.get();
    }
private:
    rss::Store& store_;rss::App& app_;std::string db_;
    std::atomic<bool> cancel_{false};std::future<void> worker_;std::mutex mutex_;std::string progress_;
};
volatile std::sig_atomic_t stop_device=0;
void stop_device_handler(int){stop_device=1;}
void device(rss::Store& store,rss::Canvas& canvas,const std::string& db){
    const auto guard=std::getenv("RSS_UI_GUARD");
    if(!guard||std::string(guard)!="1")throw std::runtime_error("设备模式必须由屏幕会话启动器运行");
    stop_device=0;
    const auto term=std::signal(SIGTERM,stop_device_handler),interrupt=std::signal(SIGINT,stop_device_handler),hangup=std::signal(SIGHUP,stop_device_handler);
    try{
        rss::KindleDevice hardware;rss::App app(store,canvas);UiTasks tasks(store,app,db);
        auto last_activity=std::chrono::steady_clock::now()-std::chrono::seconds(5);
        while(!stop_device&&!app.quit){
            if(app.dirty){app.draw();hardware.present(canvas.pixels);}
            const auto point=hardware.touch(100);
            const auto now=std::chrono::steady_clock::now();
            if(point&&now-last_activity>=std::chrono::seconds(5)){
                rss::run_bounded({"/usr/bin/lipc-set-prop","-i","com.lab126.powerd","touchScreenSaverTimeout","1"},std::chrono::seconds(2));
                last_activity=now;
            }
            try{if(point)app.tap(point->x,point->y);tasks.tick();}
            catch(const std::exception& e){app.show_error(e.what());}
        }
        // The supervising launcher restores the screen after checking the power state.
    }catch(...){std::signal(SIGTERM,term);std::signal(SIGINT,interrupt);std::signal(SIGHUP,hangup);throw;}
    std::signal(SIGTERM,term);std::signal(SIGINT,interrupt);std::signal(SIGHUP,hangup);
}
#ifdef RSS_DESKTOP
void desktop(rss::Store& store,rss::Canvas& canvas,const std::string& db){
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0)throw std::runtime_error(SDL_GetError());
    auto window=SDL_CreateWindow("Kindle RSS · 原生桌面模拟",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        canvas.width/2,canvas.height/2,SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!window){SDL_Quit();throw std::runtime_error(SDL_GetError());}
    auto renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    auto texture=renderer?SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,canvas.width,canvas.height):nullptr;
    if(!texture){if(renderer)SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();throw std::runtime_error("无法创建模拟窗口");}
    SDL_RenderSetLogicalSize(renderer,canvas.width,canvas.height);SDL_StartTextInput();
    rss::App app(store,canvas);std::vector<std::uint32_t> rgba(canvas.pixels.size());
    UiTasks tasks(store,app,db);
    try {
        while(!app.quit){
            SDL_Event event;
            if(SDL_WaitEventTimeout(&event,80)){
                try {
                    if(event.type==SDL_QUIT)app.quit=true;
                    else if(event.type==SDL_MOUSEBUTTONUP&&event.button.button==SDL_BUTTON_LEFT)app.tap(event.button.x,event.button.y);
                    else if(event.type==SDL_TEXTINPUT)app.text_input(event.text.text);
                    else if(event.type==SDL_KEYDOWN){
                        const auto key=event.key.keysym.sym;
                        if((event.key.keysym.mod&(KMOD_CTRL|KMOD_GUI))&&key==SDLK_v){auto text=SDL_GetClipboardText();if(text){app.text_input(text);SDL_free(text);}}
                        else if(key==SDLK_ESCAPE)app.back();
                        else if(key==SDLK_RIGHT||key==SDLK_PAGEDOWN)app.next();
                        else if(key==SDLK_LEFT||key==SDLK_PAGEUP)app.previous();
                        else if(key==SDLK_BACKSPACE)app.backspace();
                        else if(key==SDLK_RETURN)app.submit();
                    }else if(event.type==SDL_WINDOWEVENT)app.dirty=true;
                }catch(const std::exception& e){app.show_error(e.what());}
            }
            try{tasks.tick();}catch(const std::exception& e){app.show_error(e.what());}
            if(app.dirty){
                app.draw();for(std::size_t i=0;i<rgba.size();++i){const auto g=canvas.pixels[i];rgba[i]=0xff000000u|(g<<16)|(g<<8)|g;}
                SDL_UpdateTexture(texture,nullptr,rgba.data(),canvas.width*4);
                SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);SDL_RenderPresent(renderer);
            }
        }
    }catch(...){SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();throw;}
    SDL_StopTextInput();SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
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
                if(token=="--db")db=argv[i];else if(token=="--font")font=argv[i];else article=std::stoll(argv[i]);
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
                "  sync                 更新订阅并缓存正文\n"
                "  list                 列出本地文章\n"
                "  probe                只读检查 Kindle 屏幕和输入设备\n"
                "  device               由会话启动器启动 Kindle 原生界面\n"
                "  render FILE.pgm      渲染 1072×1448 列表；--article ID 渲染正文\n"
                "  desktop              启动原生桌面模拟器\n";return 0;
        }
        if(command=="probe"){std::cout<<rss::probe_device();return 0;}
        if(command=="device"){
            const auto launcher=std::getenv("RSS_LAUNCHER_PID");
            rss::bind_to_launcher(launcher?launcher:"");
        }
        const auto parent=std::filesystem::path(db).parent_path();if(!parent.empty())std::filesystem::create_directories(parent);
        rss::Store store(db);
        if(command=="demo")demo(store);
        else if(command=="import"){if(arg.empty())throw std::runtime_error("需要 OPML 路径");import_opml(store,arg);}
        else if(command=="export"){if(arg.empty())throw std::runtime_error("需要导出路径");write_export(store,arg);}
        else if(command=="add"){
            if(arg.rfind("http://",0)!=0&&arg.rfind("https://",0)!=0)throw std::runtime_error("需要 HTTP(S) 地址");
            rss::Feed feed;feed.url=arg;feed.title=arg;store.add_feed(feed);
        }else if(command=="sync"){
            std::atomic<bool> cancel{false};int failed=0;
            rss::sync_feeds(store,cancel,[&](const rss::SyncProgress& p){failed=p.failed;std::cout<<p.completed<<"/"<<p.total<<" "<<p.message<<"\n";});
            return failed?2:0;
        }else if(command=="list"){
            for(const auto& e:store.entries(rss::View::All))std::cout<<e.id<<"\t"<<(e.read?"已读":"未读")<<"\t"<<e.title<<"\n";
        }else if(command=="render"||command=="desktop"||command=="device"){
            rss::Canvas canvas(1072,1448,font.empty()?font_path():font);
            if(command=="device")device(store,canvas,db);
            else if(command=="render"){
                if(arg.empty())throw std::runtime_error("需要输出 PGM 路径");
                rss::App app(store,canvas);if(article)app.open(article,false);app.draw();canvas.write_pgm(arg);
            }else{
#ifdef RSS_DESKTOP
                desktop(store,canvas,db);
#else
                throw std::runtime_error("当前构建未启用桌面模拟器");
#endif
            }
        }else throw std::runtime_error("未知命令："+command);
        return 0;
    }catch(const std::exception& e){std::cerr<<"错误："<<e.what()<<"\n";return 1;}
}
