#include "status.hpp"
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
void write(const std::filesystem::path& file,const std::string& value){
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);out<<value;
}
}

int main(){
    const char* old_tz=std::getenv("TZ");
    const bool had_tz=old_tz!=nullptr;
    const std::string saved_tz=old_tz?old_tz:"";
    setenv("TZ","UTC0",1);tzset();
    const auto root=std::filesystem::temp_directory_path()/("rss-status-test-"+std::to_string(getpid()));
    std::filesystem::remove_all(root);
    const auto battery=root/"sys/class/power_supply/max77696-battery";
    const auto wifi=root/"sys/class/net/wlan0/operstate";
    write(battery/"capacity","0\n");write(battery/"status","Charging\n");write(wifi,"up\n");
    auto status=rss::read_reader_status(0,root.string());
    assert(status.battery==0&&status.charging&&status.wifi&&status.clock=="00:00");
    write(battery/"capacity","100 \t\n");write(battery/"status","Full\n");write(wifi,"down\n");
    status=rss::read_reader_status(0,root.string()+"/");
    assert(status.battery==100&&status.charging&&!status.wifi);
    for(const auto& invalid:{"","-1","+1"," 1","101","999999999999999999999999999999","12x","10\n4"}){
        write(battery/"capacity",invalid);
        assert(rss::read_reader_status(0,root.string()).battery==-1);
    }
    write(battery/"capacity",std::string(128,'1'));
    assert(rss::read_reader_status(0,root.string()).battery==-1);
    write(battery/"capacity","20\n");write(battery/"status","Discharging\n");write(wifi,"up\n");
    status=rss::read_reader_status(0,root.string());
    assert(status.battery==20&&!status.charging&&status.wifi);
    // Legacy Kindle zone files have no POSIX future rule; retain their final transition offset.
    auto be32=[](std::string& out,std::int64_t value){for(int shift:{24,16,8,0})out+=static_cast<char>(static_cast<std::uint32_t>(value)>>shift);};
    auto zone=[&](int east){
        std::string out;
        for(int width:{4,8}){
            out+="TZif2";out.append(15,'\0');for(int n:{0,0,0,2,2,8})be32(out,n);
            for(int time:{-3600,3600}){if(width==8)be32(out,time<0?-1:0);be32(out,time);}
            out+='\0';out+='\1';be32(out,-18000);out+='\0';out+='\0';be32(out,east);out+='\0';out+='\4';out.append("OLD\0NEW\0",8);
        }
        return out+"\n\n";
    };
    const auto tz=root/"var/local/system/tz";
    write(tz,zone(28800));assert(rss::read_reader_status(7200,root.string()).clock=="10:00");
    assert(rss::read_reader_status(0,root.string()).clock=="19:00");
    assert(rss::read_reader_status(-7200,root.string()).clock=="17:00");
    write(tz,zone(-12600));assert(rss::read_reader_status(7200,root.string()).clock=="22:30");
    write(tz,zone(90000));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");
    const auto valid=zone(28800);
    for(std::size_t i=0;i<valid.size();++i){write(tz,valid.substr(0,i));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");}
    write(tz,std::string(4096,'x'));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");
    std::filesystem::remove(tz);
    auto same=status;assert(status==same);same.clock="--:--";assert(status!=same);
    std::filesystem::remove_all(battery);std::filesystem::remove(wifi);
    status=rss::read_reader_status(0,root.string());
    assert(status.battery==-1&&!status.charging&&!status.wifi);
    std::filesystem::remove_all(root);
    if(had_tz)setenv("TZ",saved_tz.c_str(),1);else unsetenv("TZ");
    tzset();
}
