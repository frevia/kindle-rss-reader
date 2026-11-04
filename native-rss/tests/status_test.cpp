#include "status.hpp"
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

namespace {
void write(const std::filesystem::path& file,const std::string& value){
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);out<<value;
}
void make_lipc(const std::filesystem::path& program,const std::filesystem::path& dir){
    write(program,"#!/bin/sh\n[ \"$1\" = -i ] && [ \"$2\" = com.lab126.powerd ] || exit 9\ncase \"$3\" in battLevel) cat '"+ (dir/"lipc-level").string()+"';; isCharging) cat '"+(dir/"lipc-charging").string()+"';; *) exit 8;; esac\n");
    std::filesystem::permissions(program,std::filesystem::perms::owner_exec|std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
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
    auto local_date=[&](std::time_t time){
        std::tm local{};assert(rss::reader_local_time(time,local,root.string()));
        char out[40]{};std::strftime(out,sizeof(out),"%Y-%m-%d %H:%M",&local);return std::string(out);
    };
    write(tz,zone(28800));assert(rss::read_reader_status(7200,root.string()).clock=="10:00");
    assert(local_date(57599)=="1970-01-01 23:59");
    assert(local_date(57600)=="1970-01-02 00:00");
    assert(local_date(1735660800)=="2025-01-01 00:00");
    assert(local_date(0)=="1969-12-31 19:00");
    assert(local_date(3600)=="1970-01-01 09:00");
    std::tm overflow{};
    assert(!rss::reader_local_time(std::numeric_limits<std::time_t>::max(),overflow,root.string()));
    assert(!rss::reader_local_time(std::numeric_limits<std::time_t>::min(),overflow,root.string()));
    assert(rss::read_reader_status(0,root.string()).clock=="19:00");
    assert(rss::read_reader_status(-7200,root.string()).clock=="17:00");
    write(tz,zone(-12600));assert(rss::read_reader_status(7200,root.string()).clock=="22:30");
    assert(local_date(7200)=="1969-12-31 22:30");
    assert(local_date(12600)=="1970-01-01 00:00");
    write(tz,zone(90000));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");
    const auto valid=zone(28800);
    for(std::size_t i=0;i<valid.size();++i){write(tz,valid.substr(0,i));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");}
    write(tz,std::string(4096,'x'));assert(rss::read_reader_status(7200,root.string()).clock=="02:00");
    std::filesystem::remove(tz);
    assert(local_date(57600)=="1970-01-01 16:00");
    setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1);tzset();
    assert(local_date(1741503599)=="2025-03-09 01:59");
    assert(local_date(1741503600)=="2025-03-09 03:00");
    setenv("TZ","UTC0",1);tzset();
    auto same=status;assert(status==same);same.clock="--:--";assert(status!=same);
    std::filesystem::remove_all(battery);std::filesystem::remove(wifi);
    status=rss::read_reader_status(0,root.string());
    assert(status.battery==-1&&!status.charging&&!status.wifi);
    for(const auto* name:{"max77796-battery","bd7181x_bat","bd71827_bat"}){
        const auto alternate=root/"sys/class/power_supply"/name;
        write(alternate/"capacity","47\n");write(alternate/"status","Discharging\n");
        assert(rss::read_reader_status(0,root.string()).battery==47);
        assert(!rss::read_reader_status(0,root.string()).charging);
        write(alternate/"charging","1\n");
        assert(rss::read_reader_status(0,root.string()).charging);
        std::filesystem::remove(alternate/"charging");
        if(std::string(name)=="max77796-battery"){
            write(root/"sys/class/power_supply/max77796-charger/status","Charging\n");
            assert(rss::read_reader_status(0,root.string()).charging);
        }
        write(alternate/"capacity","101\n");
        assert(rss::read_reader_status(0,root.string()).battery==-1);
        std::filesystem::remove_all(alternate);
        std::filesystem::remove_all(root/"sys/class/power_supply/max77796-charger");
    }
    write(battery/"capacity","20\n");write(battery/"status","Discharging\n");
    write(root/"sys/class/power_supply/bd71827_bat/capacity","90\n");
    assert(rss::read_reader_status(0,root.string()).battery==20);
    make_lipc(root/"lipc-get-prop",root);
    write(root/"lipc-level","0\n");write(root/"lipc-charging","1\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==0&&status.charging);
    write(root/"lipc-level","100 \t\n");write(root/"lipc-charging","0\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==100&&!status.charging);
    for(const auto* invalid:{"","+1"," 1","101","1\n0", "999999999999999999999999999999999999999999"}){
        write(root/"lipc-level",invalid);write(root/"lipc-charging","1\n");
        status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
        assert(status.battery==20&&status.charging);
    }
    write(root/"lipc-level","50\n");write(root/"lipc-charging","2\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==50&&!status.charging);
    write(root/"lipc-level","60\n");write(root/"lipc-charging","1\n0");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==60&&!status.charging);
    write(root/"lipc-level",std::string(40,'9'));write(root/"lipc-charging","0\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==20&&!status.charging);
    for(const auto& invalid:{std::string("50\0\n",4),std::string("50\n\n"),std::string("50x"),std::string(33,'0')}){
        write(root/"lipc-level",invalid);
        assert(rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string()).battery==20);
    }
    write(root/"lipc-level",std::string(30,'0')+"50");write(root/"lipc-charging","0\n");
    assert(rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string()).battery==50);
    write(battery/"status","Charging\n");write(root/"lipc-charging","invalid\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==50&&status.charging);
    write(battery/"status","Discharging\n");
    std::filesystem::remove_all(root/"sys/class/power_supply/bd71827_bat");
    write(battery/"capacity","invalid\n");write(battery/"status","Charging\n");
    write(root/"lipc-level","42\n");write(root/"lipc-charging","invalid\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==42&&status.charging);
    write(root/"lipc-level","invalid\n");write(root/"lipc-charging","0\n");
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==-1&&!status.charging);
    write(battery/"capacity","20\n");write(battery/"status","Discharging\n");
    write(root/"lipc-get-prop","#!/bin/sh\nsleep 1\n");
    std::filesystem::permissions(root/"lipc-get-prop",std::filesystem::perms::owner_exec|std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
    const auto began=std::chrono::steady_clock::now();
    status=rss::read_reader_status(0,root.string(),(root/"lipc-get-prop").string());
    assert(status.battery==20&&!status.charging&&std::chrono::steady_clock::now()-began<std::chrono::seconds(2));
    status=rss::read_reader_status(0,root.string(),"/no/such/lipc-get-prop");
    assert(status.battery==20&&!status.charging);
    std::filesystem::remove_all(root);
    if(had_tz)setenv("TZ",saved_tz.c_str(),1);else unsetenv("TZ");
    tzset();
}
