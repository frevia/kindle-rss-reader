#include "qwen.hpp"
#include <curl/curl.h>
#include <sqlite3.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace rss {
namespace {
std::string trim(const std::string& text) {
    const auto begin=text.find_first_not_of(" \t\r\n"),end=text.find_last_not_of(" \t\r\n");
    return begin==std::string::npos?"":text.substr(begin,end-begin+1);
}
std::string json_string(const std::string& text) {
    std::string out="\"";
    const char* hex="0123456789abcdef";
    for(unsigned char c:text) {
        if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
}
QwenConfig qwen_config(const std::string& database_path) {
    QwenConfig config;
    const auto path=std::filesystem::path(database_path).parent_path()/"translation.conf";
    std::ifstream input(path,std::ios::binary);
    if(input) {
        std::string contents(16385,'\0');input.read(contents.data(),static_cast<std::streamsize>(contents.size()));
        const auto count=input.gcount();
        if(count>16384||input.bad())throw std::runtime_error("翻译配置无法读取或超过 16 KiB");
        contents.resize(static_cast<std::size_t>(count));
        if(contents.compare(0,3,"\xef\xbb\xbf")==0)contents.erase(0,3);
        for(std::size_t begin=0;begin<contents.size();) {
            const auto end=contents.find('\n',begin);
            const auto line=trim(contents.substr(begin,end==std::string::npos?end:end-begin));
            begin=end==std::string::npos?contents.size():end+1;
            if(line.empty()||line.front()=='#')continue;
            const auto equal=line.find('=');
            if(equal==std::string::npos)throw std::runtime_error("翻译配置格式错误，请使用 变量名=值");
            const auto key=trim(line.substr(0,equal)),value=trim(line.substr(equal+1));
            if(key=="RSS_QWEN_API_KEY")config.api_key=value;
            else if(key=="RSS_QWEN_ENDPOINT")config.endpoint=value;
            else throw std::runtime_error("翻译配置包含未知变量");
        }
    } else if(std::filesystem::exists(path))throw std::runtime_error("翻译配置无法读取");
    if(const auto value=std::getenv("RSS_QWEN_API_KEY"))config.api_key=value;
    if(const auto value=std::getenv("RSS_QWEN_ENDPOINT"))config.endpoint=value;
    validate_qwen_config(config);
    return config;
}
void validate_qwen_config(const QwenConfig& config) {
    if(config.api_key.empty())throw std::runtime_error("请先在 translation.conf 中设置 RSS_QWEN_API_KEY");
    if(config.api_key.size()>512||std::any_of(config.api_key.begin(),config.api_key.end(),[](unsigned char c){return c<=32||c>=127;}))
        throw std::runtime_error("翻译 API Key 格式无效");
    if(config.endpoint.size()>2048||std::any_of(config.endpoint.begin(),config.endpoint.end(),[](unsigned char c){return c<=32||c==127;}))
        throw std::runtime_error("翻译服务地址无效");
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url(curl_url(),curl_url_cleanup);
    if(!url||curl_url_set(url.get(),CURLUPART_URL,config.endpoint.c_str(),0)!=CURLUE_OK)
        throw std::runtime_error("翻译服务地址无效");
    const auto part=[&](CURLUPart field) {
        char* raw=nullptr;const auto code=curl_url_get(url.get(),field,&raw,0);
        std::unique_ptr<char,decltype(&curl_free)> value(raw,curl_free);
        return code==CURLUE_OK?std::string(raw):std::string();
    };
    const auto scheme=part(CURLUPART_SCHEME),host=part(CURLUPART_HOST);
    if(host.empty()||!part(CURLUPART_USER).empty()||!part(CURLUPART_PASSWORD).empty()||
       !part(CURLUPART_QUERY).empty()||!part(CURLUPART_FRAGMENT).empty()||
       (scheme!="https"&&!(scheme=="http"&&(host=="127.0.0.1"||host=="[::1]"))))
        throw std::runtime_error("翻译地址须为不含账号、查询参数和片段的 HTTPS 地址（本机回环地址可用 HTTP）");
}
std::string qwen_request(const std::string& text) {
    if(text.empty()||text.size()>6000)throw std::runtime_error("翻译段落为空或超过请求上限");
    return "{\"model\":\"qwen-mt-flash\",\"messages\":[{\"role\":\"user\",\"content\":"+json_string(text)+"}],"
        "\"translation_options\":{\"source_lang\":\"English\",\"target_lang\":\"Chinese\"},\"stream\":false,\"max_tokens\":8192}";
}
std::string qwen_translation(const std::string& response) {
    if(response.size()>256*1024)throw std::runtime_error("翻译响应超过大小限制");
    sqlite3* raw_db=nullptr;
    const auto opened=sqlite3_open(":memory:",&raw_db);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw_db,sqlite3_close);
    if(opened!=SQLITE_OK)throw std::runtime_error("无法解析翻译响应");
    // SQLite already ships with the app; JSON functions avoid another parser dependency.
    const char* sql="SELECT CASE WHEN json_valid(?1) THEN json_extract(?1,'$.choices[0].message.content') END,"
        "CASE WHEN json_valid(?1) THEN json_extract(?1,'$.choices[0].finish_reason') END,"
        "CASE WHEN json_valid(?1) THEN json_array_length(?1,'$.choices') END";
    sqlite3_stmt* raw_stmt=nullptr;
    const auto prepared=sqlite3_prepare_v2(db.get(),sql,-1,&raw_stmt,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw_stmt,sqlite3_finalize);
    if(prepared!=SQLITE_OK)throw std::runtime_error("翻译响应解析需要支持 JSON 的 SQLite");
    if(sqlite3_bind_text(stmt.get(),1,response.data(),static_cast<int>(response.size()),SQLITE_TRANSIENT)!=SQLITE_OK||
       sqlite3_step(stmt.get())!=SQLITE_ROW||sqlite3_column_type(stmt.get(),0)!=SQLITE_TEXT||
       sqlite3_column_type(stmt.get(),1)!=SQLITE_TEXT||sqlite3_column_int(stmt.get(),2)!=1)
        throw std::runtime_error("翻译服务返回了无效结果");
    const std::string reason(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(),1)),static_cast<std::size_t>(sqlite3_column_bytes(stmt.get(),1)));
    if(reason!="stop")throw std::runtime_error("翻译结果未完整返回，请重试本段");
    const auto size=sqlite3_column_bytes(stmt.get(),0);
    std::string text(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(),0)),static_cast<std::size_t>(size));
    if(trim(text).empty()||text.find('\0')!=std::string::npos)throw std::runtime_error("翻译服务返回了空内容或无效文本");
    return text;
}
}
