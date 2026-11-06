#include "app.hpp"
#include <algorithm>

namespace rss {
void draw_update_installation(Canvas& c,const std::string& message,int elapsed_seconds) {
    c.clear();const int w=c.width;
    c.text(32,154,"正在安装更新",48);c.rule(32,192,w-64,150);
    c.text(32,288,"备份数据 → 安装程序 → 重新打开",30);
    int y=412;
    for(const auto& line:c.wrap(message,32,w-96)){if(y>c.height-440)break;c.text(48,y,line.text,32);y+=52;}
    c.text(48,c.height-352,"已用时 "+std::to_string(elapsed_seconds)+" 秒",30,85);
    c.rule(32,c.height-288,w-64,205);y=c.height-216;
    for(const auto& line:c.wrap("安装完成后会自动打开阅读器。请勿关机或连接 USB，订阅与阅读记录会保留。",26,w-64)) {
        c.text(32,y,line.text,26,85);y+=40;
    }
}
void App::update_installing() {
    update_state_=UpdateState::Installing;update_message_="正在备份阅读数据，完成后自动安装并重启";
    update_install_started_=std::chrono::steady_clock::now();dirty=true;
}
void App::update_started(bool downloading) {
    update_state_=downloading?UpdateState::Downloading:UpdateState::Checking;
    update_message_=downloading?"正在下载更新":"正在检查 GitHub 更新";dirty=true;
}
void App::update_checked(const UpdateRelease& release) {
    update_release_=release;update_notes_page_=0;update_state_=release.available?UpdateState::Available:UpdateState::Current;
    update_message_=release.available?"发现新版本，可下载后安装":"当前已是此通道最新版本";dirty=true;
}
void App::update_progress(const std::string& message) {update_message_=message;dirty=true;}
void App::update_finished(bool cancelled,const std::string& error) {
    if(cancelled){update_state_=UpdateState::Idle;update_message_="更新已取消，当前版本仍可使用";}
    else if(!error.empty()){update_state_=UpdateState::Failed;update_message_=error;}
    else{update_state_=UpdateState::Ready;update_message_="安装包已校验，安装时会退出阅读器并重新打开";}
    dirty=true;
}
void App::update_screen() {
    if(update_state_==UpdateState::Installing) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-update_install_started_).count();
        draw_update_installation(c_,update_message_,static_cast<int>(elapsed));return;
    }
    const int w=c_.width,h=c_.height;page_header("在线升级");
    const bool running=update_state_==UpdateState::Checking||update_state_==UpdateState::Downloading;
    const bool busy=sync_state_==SyncState::Running||directory_running_;
    const bool waiting_for_background=busy&&
        (update_state_==UpdateState::Available||update_state_==UpdateState::Ready);
    const int width=w-64,footer=h-112;
    const bool has_new_version=update_release_.available&&!update_release_.version.empty();
    c_.text(32,218,"当前版本",24,85);
    c_.text(32,270,app_version(),40);
    if(has_new_version) {
        c_.text(w/2,218,"可更新至",24,85);
        c_.text(w/2,270,c_.ellipsize(update_release_.version,40,w/2-32),40);
    }
    c_.rule(32,310,width,205);

    int y=368;
    const auto message=update_message_.empty()?"检查 GitHub 上是否有新版本":update_message_;
    const auto status=c_.wrap(message,32,width);
    for(std::size_t i=0;i<std::min<std::size_t>(status.size(),3);++i) {
        const auto& line=status[i].text;
        c_.text(32,y,i==2&&status.size()>3?c_.ellipsize(line+"…",32,width):line,32);
        y+=48;
    }
    c_.text(32,y,"请连接 Wi-Fi；安装前停用首页 RSS，并预留 400 MB 可用空间。",26,85);
    c_.text(32,y+40,"订阅、收藏和阅读记录会保留。",26,85);
    y+=76;

    if(!update_release_.notes.empty()) {
        c_.rule(32,y,width,205);
        c_.text(32,y+56,"更新说明",32);
        y+=108;
        constexpr int line_height=42;
        const auto lines=c_.wrap(update_release_.notes,28,width);
        const int notes_end=footer-88; // Leave room for the background-task hint.
        auto per_page=static_cast<std::size_t>(std::max(1,(notes_end-y)/line_height+1));
        const bool paged=lines.size()>per_page;
        if(paged)per_page=static_cast<std::size_t>(std::max(1,(notes_end-100-y)/line_height+1));
        const auto pages=std::max<std::size_t>(1,(lines.size()+per_page-1)/per_page);
        update_notes_page_=std::min(update_notes_page_,pages-1);
        for(auto i=update_notes_page_*per_page;i<std::min(lines.size(),(update_notes_page_+1)*per_page);++i) {
            c_.text(32,y,lines[i].text,28,85);y+=line_height;
        }
        if(paged) {
            const int pager=notes_end-76;
            const auto count=std::to_string(update_notes_page_+1)+" / "+std::to_string(pages);
            button({32,pager,240,88},"上一页",[this]{--update_notes_page_;},false,26,Icon::Back,update_notes_page_>0);
            c_.text((w-c_.measure(count,24))/2,pager+54,count,24,85);
            button({w-272,pager,240,88},"下一页",[this]{++update_notes_page_;},false,26,Icon::Next,update_notes_page_+1<pages);
        }
    }
    c_.rule(32,footer,width,205);
    if(waiting_for_background)c_.text(32,footer-24,
        update_state_==UpdateState::Ready?"请等待当前后台任务结束，再安装更新。":"请等待当前后台任务结束，再下载更新。",24,85);
    const Rect primary{w-352,h-100,320,88};
    const bool can_download=update_state_==UpdateState::Available,can_install=update_state_==UpdateState::Ready;
    if(running)button(primary,"取消",[this]{request_="cancel-update";},false,28);
    else {
        const Rect check=can_download||can_install?Rect{32,h-100,300,88}:primary;
        button(check,can_download||can_install?"重新检查":"检查更新",[this]{request_="check-update";update_started(false);},
               !can_download&&!can_install,28);
        if(can_download)button(primary,"下载更新",[this]{request_="download-update";update_started(true);},!busy,28,Icon::None,!busy);
        if(can_install)button(primary,"安装并重启",[this]{request_="install-update";update_installing();},!busy,28,Icon::None,!busy);
    }
}
}
