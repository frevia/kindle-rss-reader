#include "process.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char** environ;
namespace rss {
void bind_to_launcher(const std::string& parent_pid){
    if(parent_pid.empty()||parent_pid.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("缺少有效的会话启动器进程号");
#ifdef __linux__
    if(parent_pid!=std::to_string(::getppid())||::getppid()<=1)
        throw std::runtime_error("会话启动器已退出");
    if(::prctl(PR_SET_PDEATHSIG,SIGKILL)!=0)
        throw std::runtime_error("无法绑定会话启动器生命周期");
    // The parent may exit between getppid and prctl.
    if(parent_pid!=std::to_string(::getppid()))
        throw std::runtime_error("会话启动器已退出");
#else
    throw std::runtime_error("会话生命周期绑定需要 Linux");
#endif
}
namespace {
void check(int error){if(error)throw std::runtime_error(std::string("系统命令失败：")+std::strerror(error));}
struct SpawnOptions {
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t files;
    SpawnOptions(){
        check(posix_spawnattr_init(&attributes));
        const int error=posix_spawn_file_actions_init(&files);
        if(error){posix_spawnattr_destroy(&attributes);check(error);}
    }
    ~SpawnOptions(){posix_spawn_file_actions_destroy(&files);posix_spawnattr_destroy(&attributes);}
};
struct Child {
    pid_t pid=-1;
    ~Child(){
        if(pid>0){::kill(-pid,SIGKILL);while(::waitpid(pid,nullptr,0)<0&&errno==EINTR){}}
    }
};
}
void run_bounded(const std::vector<std::string>& args,std::chrono::milliseconds deadline){
    if(args.empty()||args[0].empty()||args[0][0]!='/'||deadline.count()<=0||deadline>std::chrono::seconds(60))
        throw std::runtime_error("系统命令参数无效");
    std::vector<char*> argv;
    for(const auto& arg:args){
        if(arg.find('\0')!=std::string::npos)throw std::runtime_error("系统命令参数含空字节");
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    SpawnOptions options;
    check(posix_spawnattr_setflags(&options.attributes,POSIX_SPAWN_SETPGROUP));
    check(posix_spawnattr_setpgroup(&options.attributes,0));
    for(int fd=0;fd<3;++fd)check(posix_spawn_file_actions_addopen(&options.files,fd,"/dev/null",O_RDWR,0));
    Child child;
    pid_t spawned=-1;
    check(posix_spawn(&spawned,args[0].c_str(),&options.files,&options.attributes,argv.data(),environ));
    child.pid=spawned;
    const auto end=std::chrono::steady_clock::now()+deadline;
    for(;;){
        int status=0;const auto result=::waitpid(child.pid,&status,WNOHANG);
        if(result==child.pid){
            child.pid=-1;
            if(WIFEXITED(status)&&WEXITSTATUS(status)==0)return;
            throw std::runtime_error("系统命令未成功完成："+args[0]);
        }
        if(result<0&&errno!=EINTR){
            const auto error=errno;
            if(error==ECHILD)child.pid=-1;
            check(error);
        }
        if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("系统命令超时："+args[0]);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
