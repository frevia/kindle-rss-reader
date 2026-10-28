#include "status.hpp"
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <optional>

namespace rss {
namespace {
std::string path(const std::string& root,const char* relative){
    return root+(root.empty()||root.back()=='/'?"":"/")+relative;
}
std::string read_small(const std::string& filename,std::size_t limit=128){
    char buffer[4096];
    auto* file=std::fopen(filename.c_str(),"r");
    if(!file)return {};
    const auto size=std::fread(buffer,1,limit,file);
    const bool too_long=size==limit;
    std::fclose(file);
    return too_long?std::string{}:std::string(buffer,size);
}
// Kindle writes TZif2 with an empty future-rule footer; musl otherwise falls back to UTC after the last transition.
std::optional<int> kindle_offset(const std::string& data,std::int64_t now){
    if(data.size()<46||data.compare(0,4,"TZif")||data.substr(data.size()-2)!="\n\n")return {};
    auto number=[&](std::size_t p,int width){
        std::uint64_t value=0;for(int i=0;i<width;++i)value=(value<<8)|static_cast<unsigned char>(data[p+i]);
        return value;
    };
    auto signed_number=[&](std::size_t p,int width)->std::int64_t{
        const auto value=number(p,width),mask=width==8?UINT64_MAX:UINT32_MAX;
        return static_cast<unsigned char>(data[p])&128?-1-static_cast<std::int64_t>(value^mask):static_cast<std::int64_t>(value);
    };
    std::size_t header=0;int width=4;
    for(int block=0;block<2;++block){
        if(header+44>data.size()||data.compare(header,4,"TZif"))return {};
        std::size_t count[6];
        for(int i=0;i<6;++i){count[i]=number(header+20+i*4,4);if(count[i]>4096)return {};}
        const auto transitions=count[3],types=count[4];
        if(!types||types>256)return {};
        const auto end=header+44+transitions*(width+1)+types*6+count[5]+count[2]*(width+4)+count[0]+count[1];
        if(end>data.size())return {};
        if(block==0){if(data[4]!='2'&&data[4]!='3')return {};header=end;width=8;continue;}
        if(end+2!=data.size())return {};
        const auto times=header+44,indices=times+transitions*width,records=indices+transitions;
        std::size_t selected=0;
        while(selected+1<types&&data[records+selected*6+4])++selected;
        std::optional<std::int64_t> previous;
        for(std::size_t i=0;i<transitions;++i){
            const auto time=signed_number(times+i*width,width);
            const auto type=static_cast<unsigned char>(data[indices+i]);
            if(type>=types||(previous&&time<=*previous))return {};
            previous=time;if(time<=now)selected=type;
        }
        const auto offset=signed_number(records+selected*6,4);
        if(offset < -86400||offset > 86400)return {};
        return static_cast<int>(offset);
    }
    return {};
}

int capacity(const std::string& value){
    std::size_t i=0;
    if(value.empty()||value[i]<'0'||value[i]>'9')return -1;
    int result=0;
    for(;i<value.size()&&value[i]>='0'&&value[i]<='9';++i){
        result=result*10+(value[i]-'0');
        if(result>100)return -1;
    }
    for(;i<value.size();++i)if(!std::isspace(static_cast<unsigned char>(value[i])))return -1;
    return result;
}
}

bool ReaderStatus::operator==(const ReaderStatus& other) const{
    return battery==other.battery&&charging==other.charging&&wifi==other.wifi&&clock==other.clock;
}
bool ReaderStatus::operator!=(const ReaderStatus& other) const{return !(*this==other);}

ReaderStatus read_reader_status(std::time_t now,const std::string& root){
    ReaderStatus status;
    status.battery=capacity(read_small(path(root,"sys/class/power_supply/max77696-battery/capacity")));
    const auto supply=read_small(path(root,"sys/class/power_supply/max77696-battery/status"));
    status.charging=supply=="Charging\n"||supply=="Charging"||supply=="Full\n"||supply=="Full";
    const auto network=read_small(path(root,"sys/class/net/wlan0/operstate"));
    status.wifi=network=="up\n"||network=="up";
    std::tm local{};
    char clock[6]="--:--";
    if(localtime_r(&now,&local))std::strftime(clock,sizeof(clock),"%H:%M",&local);
    if(const auto offset=kindle_offset(read_small(path(root,"var/local/system/tz"),4096),now)){
        auto seconds=(static_cast<std::int64_t>(now)%86400+*offset)%86400;
        if(seconds<0)seconds+=86400;
        std::snprintf(clock,sizeof(clock),"%02d:%02d",static_cast<int>(seconds/3600),static_cast<int>(seconds/60%60));
    }
    status.clock=clock;
    return status;
}
}
