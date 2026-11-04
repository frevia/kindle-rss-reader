#include "process.hpp"
#ifdef NDEBUG
#error "process_test requires assertions; compile with -UNDEBUG"
#endif
#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <fstream>
#include <future>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
pid_t wait_for_marker(const char* path, pid_t* pgid) {
    using namespace std::chrono;
    const auto deadline=steady_clock::now()+seconds(2);
    while(steady_clock::now()<deadline){
        std::ifstream marker(path);pid_t pid=-1,group=-1;
        if(marker>>pid>>group){*pgid=group;return pid;}
        std::this_thread::sleep_for(milliseconds(10));
    }
    return -1;
}

void kill_owned_group(pid_t pid,pid_t pgid){
    if(pgid>0)assert(kill(-pgid,SIGKILL)==0||errno==ESRCH);
    if(pid>0)assert(kill(pid,SIGKILL)==0||errno==ESRCH);
}

bool pipe_reached_eof(int fd,int timeout_ms){
    pollfd channel{fd,POLLIN|POLLHUP,0};int ready;
    do{ready=poll(&channel,1,timeout_ms);}while(ready<0&&errno==EINTR);
    assert(ready>=0);
    if(ready==0)return false;
    char byte;ssize_t count;
    do{count=read(fd,&byte,1);}while(count<0&&errno==EINTR);
    assert(count>=0);
    return count==0;
}

void bounded_reader_death(bool capture){
    using namespace std::chrono;
    int fifo[2];assert(pipe(fifo)==0);
    // Keep fd 8 free for the simulated launch.sh session FIFO.
    for(int& fd:fifo)if(fd==8){const int moved=fcntl(fd,F_DUPFD,9);assert(moved>=0);assert(close(fd)==0);fd=moved;}
    char marker_path[]="/tmp/rss-process-death.XXXXXX";
    const int marker_fd=mkstemp(marker_path);assert(marker_fd>=0);assert(close(marker_fd)==0);assert(unlink(marker_path)==0);
    const pid_t reader=fork();assert(reader>=0);
    if(reader==0){
        assert(close(fifo[0])==0);
        if(fifo[1]!=8){if(dup2(fifo[1],8)<0)_exit(10);assert(close(fifo[1])==0);}
        try{
            const std::string shell="sleep 30 & printf '%s %s\\n' \"$$\" \"$$\" > \"$1\"; wait";
            if(capture)(void)rss::capture_bounded({"/bin/sh","-c",shell,"death-test",marker_path},seconds(30),32);
            else rss::run_bounded({"/bin/sh","-c",shell,"death-test",marker_path},seconds(30));
            _exit(0);
        }catch(...){_exit(11);}
    }
    assert(close(fifo[1])==0);
    pid_t tool_pid=-1,tool_pgid=-1;
    tool_pid=wait_for_marker(marker_path,&tool_pgid);
    const bool started=tool_pid>0&&tool_pgid>0;
    assert(kill(reader,SIGKILL)==0||errno==ESRCH);
    const bool eof=started&&pipe_reached_eof(fifo[0],2000);
    if(!eof)kill_owned_group(tool_pid,tool_pgid);
    int status=0;pid_t waited;do{waited=waitpid(reader,&status,0);}while(waited<0&&errno==EINTR);
    assert(waited==reader);
    assert(close(fifo[0])==0);assert(unlink(marker_path)==0||errno==ENOENT);
    assert(started&&eof);
    std::cout<<(capture?"capture":"run")<<"_reader_sigkill_recovery=passed"<<std::endl;
}

void bounded_timeout_cleans_inherited_pipe(){
    using namespace std::chrono;
    int fifo[2];assert(pipe(fifo)==0);
    for(int& fd:fifo)if(fd==8){const int moved=fcntl(fd,F_DUPFD,9);assert(moved>=0);assert(close(fd)==0);fd=moved;}
    char marker_path[]="/tmp/rss-process-timeout.XXXXXX";
    const int marker_fd=mkstemp(marker_path);assert(marker_fd>=0);assert(close(marker_fd)==0);assert(unlink(marker_path)==0);
    const pid_t reader=fork();assert(reader>=0);
    if(reader==0){
        assert(close(fifo[0])==0);
        if(fifo[1]!=8){if(dup2(fifo[1],8)<0)_exit(10);assert(close(fifo[1])==0);}
        try{rss::run_bounded({"/bin/sh","-c","sleep 30 & printf '%s %s\\n' \"$$\" \"$$\" > \"$1\"; wait","timeout-test",marker_path},milliseconds(150));_exit(11);}
        catch(const std::runtime_error& error){_exit(std::string(error.what()).find("超时")!=std::string::npos?0:12);}
        catch(...){_exit(13);}
    }
    assert(close(fifo[1])==0);
    pid_t tool_pid=-1,tool_pgid=-1;
    tool_pid=wait_for_marker(marker_path,&tool_pgid);
    const bool started=tool_pid>0&&tool_pgid>0;
    const bool eof=started&&pipe_reached_eof(fifo[0],2000);
    if(!eof){kill_owned_group(tool_pid,tool_pgid);assert(kill(reader,SIGKILL)==0||errno==ESRCH);}
    int status=0;pid_t waited;do{waited=waitpid(reader,&status,0);}while(waited<0&&errno==EINTR);
    assert(waited==reader);
    assert(close(fifo[0])==0);assert(unlink(marker_path)==0||errno==ENOENT);
    assert(started&&eof&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    std::cout<<"timeout_inherited_pipe_cleanup=passed"<<std::endl;
}
}

int main(){
    using namespace std::chrono;
    for(const auto& pid:{"", "1", "abc", "12 3"}){
        bool rejected=false;
        try{rss::bind_to_launcher(pid);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected);
    }
    bounded_reader_death(false);
    bounded_reader_death(true);
    bounded_timeout_cleans_inherited_pipe();
    auto parallel=std::async(std::launch::async,[]{return rss::capture_bounded({"/bin/sh","-c","sleep 1; printf one"},seconds(3),3);});
    assert(rss::capture_bounded({"/bin/sh","-c","sleep 1; printf two"},seconds(3),3)=="two");
    assert(parallel.get()=="one");
#ifdef __linux__
    // A grandchild keeps the pipe open; only kernel parent-death termination closes it.
    int pipefd[2];assert(pipe(pipefd)==0);
    const auto launcher=fork();assert(launcher>=0);
    if(launcher==0){
        close(pipefd[0]);const auto owner=getpid();
        const auto reader=fork();if(reader<0)_exit(2);
        if(reader==0){
            rss::bind_to_launcher(std::to_string(owner));
            const auto pid=getpid();
            if(write(pipefd[1],&pid,sizeof(pid))!=sizeof(pid))_exit(3);
            for(;;)pause();
        }
        close(pipefd[1]);for(;;)pause();
    }
    close(pipefd[1]);pollfd channel{pipefd[0],POLLIN,0};
    const auto ready=poll(&channel,1,2000);
    pid_t reader=-1;
    if(ready>0&&channel.revents&POLLIN)assert(read(pipefd[0],&reader,sizeof(reader))==sizeof(reader));
    kill(launcher,SIGKILL);int launcher_status=0;assert(waitpid(launcher,&launcher_status,0)==launcher);
    channel.revents=0;const auto closed=poll(&channel,1,2000);
    const bool terminated=closed>0&&(channel.revents&POLLHUP);
    if(!terminated&&reader>0)kill(reader,SIGKILL);
    close(pipefd[0]);assert(reader>0&&terminated);
    std::cout<<"parent_death_protection=passed"<<std::endl;
#endif
    rss::run_bounded({"/bin/sh","-c","exit 0"},seconds(1));
    std::cout<<"spawn_shell=passed"<<std::endl;
    for(const auto& args:std::vector<std::vector<std::string>>{
        {"/bin/sh","-c","exit 7"},{"/no/such/kindle-command"},{"relative"},{"/bin/sh",std::string("x\0y",3)}}){
        bool failed=false;try{rss::run_bounded(args,seconds(1));}catch(const std::runtime_error&){failed=true;}assert(failed);
    }
    // A shell spawning sleep exercises termination of the entire command group.
    const auto start=steady_clock::now();bool timed_out=false;
    try{rss::run_bounded({"/bin/sh","-c","sleep 10; exit 0"},milliseconds(80));}
    catch(const std::runtime_error& e){timed_out=std::string(e.what()).find("超时")!=std::string::npos;}
    assert(timed_out);assert(steady_clock::now()-start<seconds(2));
    int status=0;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
    std::cout<<"timeout_and_reaping=passed"<<std::endl;
    // Metacharacters are a literal argument unless a caller explicitly chooses a shell.
    rss::run_bounded({"/bin/sh","-c","[ \"$1\" = \"$2\" ]","literal-argument-test","$(exit 9); *","$(exit 9); *"},seconds(1));
    std::cout<<"literal_arguments=passed"<<std::endl;
    assert(rss::capture_bounded({"/bin/sh","-c","printf abc; printf def"},seconds(1),6)=="abcdef");
    assert(rss::capture_bounded({"/bin/sh","-c","exit 0"},seconds(1),0).empty());
    assert(rss::capture_bounded({"/bin/sh","-c","printf '%s' \"$1\"","capture-literal","$(exit 9); *"},seconds(1),32)=="$(exit 9); *");
    for(const auto& args:std::vector<std::vector<std::string>>{
        {"/bin/sh","-c","exit 9"},{"/no/such/capture-command"},{"/bin/sh","-c","printf 123456"}}){
        bool failed=false;try{rss::capture_bounded(args,seconds(1),args[0]=="/bin/sh"&&args[2]=="printf 123456"?5:32);}catch(const std::runtime_error&){failed=true;}assert(failed);
    }
    for(const auto& command:{"sleep 10","exec 1>&-; sleep 10"}){
        const auto began=steady_clock::now();bool failed=false;
        try{rss::capture_bounded({"/bin/sh","-c",command},milliseconds(80),32);}catch(const std::runtime_error&){failed=true;}
        assert(failed&&steady_clock::now()-began<seconds(2));
    }
    const auto background_exit_start=steady_clock::now();
    assert(rss::capture_bounded({"/bin/sh","-c","(sleep 10) & exit 0"},seconds(1),32).empty());
    assert(steady_clock::now()-background_exit_start<seconds(2));
    const auto streaming_start=steady_clock::now();bool streaming_timed_out=false;
    try{rss::capture_bounded({"/bin/sh","-c","while :; do printf x; sleep 1; done"},milliseconds(80),32);}
    catch(const std::runtime_error& e){streaming_timed_out=std::string(e.what()).find("超时")!=std::string::npos;}
    assert(streaming_timed_out&&steady_clock::now()-streaming_start<seconds(2));
    // A closed stdout is not proof that the command's descendants have stopped.
    char marker[]="/tmp/rss-process-test.XXXXXX";const int marker_fd=mkstemp(marker);assert(marker_fd>=0);
    close(marker_fd);unlink(marker);
    assert(rss::capture_bounded({"/bin/sh","-c","(exec 1>&-; sleep 1; : >\"$1\") & printf ok","descendant",marker},seconds(1),2)=="ok");
    std::this_thread::sleep_for(milliseconds(1200));
    const bool leaked=std::ifstream(marker).good();unlink(marker);assert(!leaked);
    const auto closed_stdio=fork();assert(closed_stdio>=0);
    if(closed_stdio==0){
        close(0);close(1);close(2);
        try{_exit(rss::capture_bounded({"/bin/sh","-c","printf ok"},seconds(1),2)=="ok"?0:2);}catch(...){_exit(3);}
    }
    int closed_status=0;assert(waitpid(closed_stdio,&closed_status,0)==closed_stdio);
    assert(WIFEXITED(closed_status)&&WEXITSTATUS(closed_status)==0);
    assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);
    std::cout<<"bounded_capture=passed"<<std::endl;
}
