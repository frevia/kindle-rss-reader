#include "process.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <ctime>
#include <dirent.h>
#include <cstdlib>
#include <climits>
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
struct Child {
    pid_t pid=-1;
    int lifetime=-1;
    bool reaped=false;
    ~Child(){
        if(lifetime>=0)::close(lifetime);
        if(pid>0&&!reaped)while(::waitpid(pid,nullptr,0)<0&&errno==EINTR){}
    }
};
struct Fd {
    int value=-1;
    ~Fd(){if(value>=0)::close(value);}
};
int above_stdio(int fd){
    if(fd>2)return fd;
    const int moved=::fcntl(fd,F_DUPFD_CLOEXEC,3);if(moved<0)check(errno);::close(fd);return moved;
}
void channel(Fd& reader,Fd& writer){
    int descriptors[2];if(::pipe(descriptors)<0)check(errno);
    reader.value=descriptors[0];writer.value=descriptors[1];
    reader.value=above_stdio(reader.value);writer.value=above_stdio(writer.value);
    for(int fd:{reader.value,writer.value}){
        const int flags=::fcntl(fd,F_GETFD);if(flags<0)check(errno);
        if(::fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0)check(errno);
    }
}
void validate(const std::vector<std::string>& args,std::chrono::milliseconds deadline){
    if(args.empty()||args[0].empty()||args[0][0]!='/'||deadline.count()<=0||deadline>std::chrono::seconds(60))
        throw std::runtime_error("系统命令参数无效");
    for(const auto& arg:args)if(arg.find('\0')!=std::string::npos)throw std::runtime_error("系统命令参数含空字节");
}
std::vector<char*> arguments(const std::vector<std::string>& args){
    std::vector<char*> argv;for(const auto& arg:args)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);return argv;
}
std::vector<int> inherited_fds(){
#ifdef __linux__
    DIR* directory=::opendir("/proc/self/fd");
#else
    DIR* directory=::opendir("/dev/fd");
#endif
    if(!directory)check(errno);
    std::vector<int> descriptors;
    while(auto* entry=::readdir(directory)){
        char* end=nullptr;const auto value=std::strtol(entry->d_name,&end,10);
        if(end!=entry->d_name&&!*end&&value>2&&value<=INT_MAX)descriptors.push_back(static_cast<int>(value));
    }
    ::closedir(directory);return descriptors;
}
std::int64_t monotonic_ms() noexcept {
    timespec now{};if(::clock_gettime(CLOCK_MONOTONIC,&now)<0)return -1;
    return static_cast<std::int64_t>(now.tv_sec)*1000+now.tv_nsec/1000000;
}
// After fork, use only prebuilt data and async-signal-safe calls: the reader has worker threads.
[[noreturn]] void supervise(char* const* argv,int alive,int owner_end,int null_fd,int output,int output_read,bool stderr_output,std::int64_t end,const int* descriptors,std::size_t count,bool session_fifo) noexcept {
    ::close(owner_end);if(output_read>=0)::close(output_read);
    // Keep only our own handles and the session FIFO; sibling lifetime pipes must not form a cycle.
    for(std::size_t i=0;i<count;++i){const int fd=descriptors[i];
        if(fd!=alive&&fd!=null_fd&&fd!=output&&!(fd==8&&session_fifo))::close(fd);
    }
    const pid_t tool=::fork();
    if(tool<0)_exit(127);
    if(tool==0){
        ::close(alive);
        if(::setpgid(0,0)<0)_exit(127);
        if(::dup2(null_fd,0)<0||::dup2(output>=0?output:null_fd,1)<0||::dup2(stderr_output?output:null_fd,2)<0)_exit(127);
        ::close(null_fd);if(output>=0)::close(output);
        ::execve(argv[0],argv,environ);_exit(127);
    }
    ::close(null_fd);if(output>=0)::close(output);
    // Both sides set the group, so owner death cannot race the tool's group creation.
    const bool group_ok=::setpgid(tool,tool)==0||errno==EACCES||errno==ESRCH;
    int status=0,reason=127;bool reaped=false;
    if(group_ok)for(;;){
        const auto waited=::waitpid(tool,&status,WNOHANG);
        if(waited==tool){reaped=true;reason=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);break;}
        if(waited<0&&errno!=EINTR)break;
        const auto now=monotonic_ms();if(now<0)break;
        if(now>=end){reason=124;break;}
        pollfd parent{alive,POLLIN,0};
        const int ready=::poll(&parent,1,static_cast<int>(end-now<10?end-now:10));
        if(ready<0&&errno!=EINTR)break;
        if(ready>0){reason=125;break;}
    }
    ::kill(-tool,SIGKILL);if(!group_ok)::kill(tool,SIGKILL);
    if(!reaped)while(::waitpid(tool,&status,0)<0&&errno==EINTR){}
    _exit(reason);
}
void spawn(Child& child,const std::vector<std::string>& args,std::chrono::milliseconds deadline,int output=-1,int output_read=-1,bool stderr_output=false){
    auto argv=arguments(args);Fd alive,owner_end,null_fd;channel(alive,owner_end);
    null_fd.value=::open("/dev/null",O_RDWR|O_CLOEXEC);if(null_fd.value<0)check(errno);
    null_fd.value=above_stdio(null_fd.value);
    const auto descriptors=inherited_fds();
    const int session_flags=::fcntl(8,F_GETFD);
    const bool session_fifo=session_flags>=0&&!(session_flags&FD_CLOEXEC);
    const auto now=monotonic_ms();if(now<0)check(errno);
    const auto pid=::fork();if(pid<0)check(errno);
    if(pid==0)supervise(argv.data(),alive.value,owner_end.value,null_fd.value,output,output_read,stderr_output,now+deadline.count(),descriptors.data(),descriptors.size(),session_fifo);
    child.pid=pid;child.lifetime=owner_end.value;owner_end.value=-1;
}
}
void run_bounded(const std::vector<std::string>& args,std::chrono::milliseconds deadline){
    validate(args,deadline);
    Child child;
    spawn(child,args,deadline);
    const auto end=std::chrono::steady_clock::now()+deadline;
    for(;;){
        int status=0;const auto result=::waitpid(child.pid,&status,WNOHANG);
        if(result==child.pid){
            child.reaped=true;
            child.pid=-1;
            if(WIFEXITED(status)&&WEXITSTATUS(status)==0)return;
            if(WIFEXITED(status)&&WEXITSTATUS(status)==124)throw std::runtime_error("系统命令超时："+args[0]);
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
std::string capture_bounded(const std::vector<std::string>& args,std::chrono::milliseconds deadline,std::size_t max_bytes,bool include_stderr){
    validate(args,deadline);
    if(max_bytes>1024*1024)throw std::runtime_error("系统命令输出上限无效");
    Fd read_end,write_end;channel(read_end,write_end);
    const int flags=::fcntl(read_end.value,F_GETFL);if(flags<0)check(errno);if(::fcntl(read_end.value,F_SETFL,flags|O_NONBLOCK)<0)check(errno);
    Child child;spawn(child,args,deadline,write_end.value,read_end.value,include_stderr);
    ::close(write_end.value);write_end.value=-1;
    const auto end=std::chrono::steady_clock::now()+deadline;
    std::string output;bool eof=false,exited=false;int status=0;
    char buffer[4096];
    while(!eof||!exited){
        for(;;){
            if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("系统命令超时："+args[0]);
            const auto count=::read(read_end.value,buffer,sizeof(buffer));
            if(count>0){if(output.size()+static_cast<std::size_t>(count)>max_bytes)throw std::runtime_error("系统命令输出超限");output.append(buffer,static_cast<std::size_t>(count));continue;}
            if(count==0){eof=true;break;}
            if(errno==EINTR)continue;
            if(errno==EAGAIN||errno==EWOULDBLOCK)break;
            check(errno);
        }
        if(!exited){const auto result=::waitpid(child.pid,&status,WNOHANG);if(result==child.pid){exited=true;child.reaped=true;}else if(result<0&&errno!=EINTR){if(errno==ECHILD){child.reaped=true;throw std::runtime_error("无法回收系统命令进程");}else check(errno);}}
        if(eof&&exited)break;
        if(std::chrono::steady_clock::now()>=end)throw std::runtime_error("系统命令超时："+args[0]);
        pollfd descriptor{eof?-1:read_end.value,POLLIN|POLLHUP,0};
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count();
        const int wait=static_cast<int>(std::min<std::int64_t>(10,std::max<std::int64_t>(1,remaining)));
        const int result=::poll(&descriptor,1,wait);if(result<0&&errno!=EINTR)check(errno);
    }
    if(WIFEXITED(status)&&WEXITSTATUS(status)==124)throw std::runtime_error("系统命令超时："+args[0]);
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("系统命令未成功完成："+args[0]);
    return output;
}
}
