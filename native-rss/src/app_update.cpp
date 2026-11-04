#include "app.hpp"
#include <algorithm>

namespace rss {
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
    const int w=c_.width,h=c_.height;page_header("在线升级");
    const bool running=update_state_==UpdateState::Checking||update_state_==UpdateState::Downloading;
    const bool busy=sync_state_==SyncState::Running;
    c_.text(32,240,"当前版本："+app_version()+" · Kindle兼容版",32);
    if(!update_release_.version.empty())c_.text(32,304,"最新版本："+update_release_.version,32);
    int y=380;
    const auto message=update_message_.empty()?"从 GitHub Releases 获取更新；beta 版会包含后续 beta。请先连接 Wi-Fi，升级前停用首页 RSS，并保留至少 400 MB 可用空间。":update_message_;
    for(const auto& line:c_.wrap(message,28,w-64)){if(y>600)break;c_.text(32,y,line.text,28,85);y+=44;}
    c_.rule(32,640,w-64,205);
    c_.text(32,704,"更新说明",32);y=768;
    const auto notes=update_release_.notes.empty()?"检查更新后显示发布说明。订阅、收藏与阅读记录会保留。":update_release_.notes;
    const auto lines=c_.wrap(notes,26,w-64);const auto per_page=static_cast<std::size_t>(std::max(1,(h-1080)/40));
    const auto pages=std::max<std::size_t>(1,(lines.size()+per_page-1)/per_page);
    update_notes_page_=std::min(update_notes_page_,pages-1);
    for(auto i=update_notes_page_*per_page;i<std::min(lines.size(),(update_notes_page_+1)*per_page);++i){c_.text(32,y,lines[i].text,26,85);y+=40;}
    if(pages>1) {
        button({32,h-300,240,88},"上一页",[this]{if(update_notes_page_)--update_notes_page_;},false,26,Icon::None,update_notes_page_>0);
        c_.text(w/2-54,h-244,std::to_string(update_notes_page_+1)+" / "+std::to_string(pages),24,85);
        button({w-272,h-300,240,88},"下一页",[this]{++update_notes_page_;},false,26,Icon::None,update_notes_page_+1<pages);
    }
    c_.rule(32,h-204,w-64,205);
    if(busy)c_.text(32,h-152,"请先等待同步或翻译完成，再升级。",24,85);
    if(running)button({32,h-100,300,88},"取消",[this]{request_="cancel-update";},false,28);
    else {
        button({32,h-100,300,88},"检查更新",[this]{request_="check-update";update_started(false);},false,28,Icon::None,!busy);
        if(update_state_==UpdateState::Available)button({w-352,h-100,320,88},"下载更新",[this]{request_="download-update";update_started(true);},true,28,Icon::None,!busy);
        if(update_state_==UpdateState::Ready)button({w-352,h-100,320,88},"安装并重启",[this]{request_="install-update";},true,28,Icon::None,!busy);
    }
}
}
