#include "process.hpp"
#ifdef NDEBUG
#error "process_test requires assertions; compile with -UNDEBUG"
#endif
#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#ifdef __linux__
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

int main(){
    using namespace std::chrono;
    for(const auto& pid:{"", "1", "abc", "12 3"}){
        bool rejected=false;
        try{rss::bind_to_launcher(pid);}catch(const std::runtime_error&){rejected=true;}
        assert(rejected);
    }
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
}
