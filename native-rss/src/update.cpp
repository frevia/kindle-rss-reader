#include "update.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rss {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t package_limit = 128ULL*1024*1024, expanded_limit = 256ULL*1024*1024;
const std::string repository = "https://github.com/frevia/kindle-rss-reader/releases/download/";
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
void require(bool condition, const std::string& message) { if(!condition)fail(message); }
std::vector<std::vector<std::string>> query(const std::string& json, const char* sql) {
    require(json.size()<=2*1024*1024,"更新清单过大");
    sqlite3* raw=nullptr;const int opened=sqlite3_open(":memory:",&raw);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    require(opened==SQLITE_OK,"无法读取更新清单");
    sqlite3_stmt* statement=nullptr;
    const int prepared=sqlite3_prepare_v2(db.get(),sql,-1,&statement,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(statement,sqlite3_finalize);
    require(prepared==SQLITE_OK&&sqlite3_bind_text(stmt.get(),1,json.data(),static_cast<int>(json.size()),SQLITE_TRANSIENT)==SQLITE_OK,"无法解析更新清单");
    std::vector<std::vector<std::string>> rows;int result;
    while((result=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        require(rows.size()<2048,"更新文件过多");std::vector<std::string> row;
        for(int i=0;i<sqlite3_column_count(stmt.get());++i) {
            const auto* value=sqlite3_column_text(stmt.get(),i);
            row.emplace_back(value?reinterpret_cast<const char*>(value):"",static_cast<std::size_t>(sqlite3_column_bytes(stmt.get(),i)));
            require(row.back().find('\0')==std::string::npos,"更新清单含无效文字");
        }
        rows.push_back(std::move(row));
    }
    require(result==SQLITE_DONE,"更新清单 JSON 无效");return rows;
}
std::array<unsigned,4> version_parts(const std::string& version) {
    require(version.size()>=6&&version.size()<=40&&version[0]=='v',"更新版本号无效");
    std::array<unsigned,4> parts{0,0,0,1000000};std::size_t offset=1;
    for(unsigned i=0;i<3;++i) {
        const auto begin=offset;
        while(offset<version.size()&&version[offset]>='0'&&version[offset]<='9') {
            require(offset-begin<6,"更新版本号无效");parts[i]=parts[i]*10+static_cast<unsigned>(version[offset++]-'0');
        }
        require(offset>begin&&(offset-begin==1||version[begin]!='0'),"更新版本号无效");
        if(i<2)require(offset<version.size()&&version[offset++]=='.',"更新版本号无效");
    }
    if(offset<version.size()) {
        require(version.compare(offset,6,"-beta.")==0,"更新版本号无效");offset+=6;
        const auto begin=offset;parts[3]=0;
        while(offset<version.size()&&version[offset]>='0'&&version[offset]<='9') {
            require(offset-begin<6,"更新版本号无效");parts[3]=parts[3]*10+static_cast<unsigned>(version[offset++]-'0');
        }
        require(offset>begin&&version[begin]!='0',"更新版本号无效");
    }
    require(offset==version.size(),"更新版本号无效");return parts;
}
std::string read(const fs::path& path,std::size_t limit=2*1024*1024) {
    std::ifstream in(path,std::ios::binary);require(static_cast<bool>(in),"无法读取升级文件");
    std::string data(limit+1,'\0');in.read(data.data(),static_cast<std::streamsize>(data.size()));
    require(!in.bad()&&static_cast<std::size_t>(in.gcount())<=limit,"升级文件过大或读取失败");data.resize(static_cast<std::size_t>(in.gcount()));return data;
}
bool digest_valid(const std::string& digest) {
    return digest.size()==64&&digest.find_first_not_of("0123456789abcdef")==std::string::npos;
}
std::string hash(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);require(static_cast<bool>(in),"无法校验升级文件");
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    require(context&&EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)==1,"校验初始化失败");
    std::array<char,65536> buffer{};
    while(in) {in.read(buffer.data(),buffer.size());require(EVP_DigestUpdate(context.get(),buffer.data(),static_cast<std::size_t>(in.gcount()))==1,"文件校验失败");}
    require(in.eof(),"文件读取失败");std::array<unsigned char,32> bytes{};unsigned size=0;
    require(EVP_DigestFinal_ex(context.get(),bytes.data(),&size)==1&&size==32,"文件校验失败");
    std::string out;for(auto c:bytes){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;
}
void durable(const fs::path& path) {
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW);require(fd>=0,"无法同步升级文件");
    const int result=::fsync(fd);::close(fd);require(result==0,"升级文件无法写入磁盘");
}
void write(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(text.data(),static_cast<std::streamsize>(text.size()));out.close();
    require(static_cast<bool>(out),"升级文件写入失败，请检查空间");durable(path);
}
void safe_path(const fs::path& root,const fs::path& relative) {
    require(!relative.empty()&&!relative.is_absolute(),"无效升级路径");
    fs::path path=root;
    require(!fs::is_symlink(fs::symlink_status(root))&&fs::is_directory(root),"升级根目录无效");
    for(const auto& part:relative) {
        require(part!=".."&&part!="."&&!part.empty(),"升级路径越界");path/=part;
        const auto status=fs::symlink_status(path);
        require(!fs::is_symlink(status),"升级目录含符号链接，请先手动修复");
        require(!fs::exists(status)||fs::is_regular_file(status)||fs::is_directory(status),"升级目录含特殊文件");
    }
}
fs::path update_folder(const fs::path& root) {
    safe_path(root,"native-rss/.update");
    for(const auto* name:{"ready","runner","journal","journal.tmp","result","database.sqlite","package.zip","stage","backup"})
        safe_path(root,fs::path("native-rss/.update")/name);
    return root/"native-rss/.update";
}
bool allowed(const std::string& name) {
    if(name.empty()||name.size()>240||name.front()=='/'||name.back()=='/'||name.find('\\')!=std::string::npos||
       std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32||c==127;}))return false;
    for(const auto& part:fs::path(name))if(part==".."||part=="."||part.empty())return false;
    if(name.find("//")!=std::string::npos)return false;
    static const std::set<std::string> files={"native-rss/bin/kindle-rss","native-rss/bin/kindle-rss-home",
        "native-rss/launch.sh","native-rss/diagnose.sh","native-rss/home.sh","native-rss/book-cover.png",
        "native-rss/INSTALL.txt","native-rss/README.md","native-rss/LICENSE","native-rss/COPYING",
        "native-rss/THIRD_PARTY_NOTICES.md","native-rss/BUILD.json","native-rss/DOCUMENTATION.json","native-rss/MANIFEST.json","native-rss/UPDATE.json",
        "extensions/native-rss/config.xml","extensions/native-rss/menu.json","documents/RSS阅读器.sh","documents/RSS设备诊断.sh"};
    if(files.count(name))return true;
    for(const auto* prefix:{"native-rss/fonts/","native-rss/certs/","native-rss/licenses/","native-rss/docs/","native-rss/home/"})
        if(name.rfind(prefix,0)==0&&name.size()>std::char_traits<char>::length(prefix))return true;
    return false;
}
struct Transfer {
    std::ofstream* file=nullptr;std::string body;std::uint64_t count=0,limit=0;bool failed=false;
    const std::atomic<bool>* cancel=nullptr;UpdateProgress progress;std::uint64_t expected=0;
    std::string phase;std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now(),last_report=started,last_data=started;
};
std::size_t receive(char* data,std::size_t size,std::size_t count,void* opaque) noexcept {
    auto& t=*static_cast<Transfer*>(opaque);const auto length=size*count;
    try {
        if(t.cancel->load()||length>t.limit-t.count){t.failed=true;return 0;}
        if(t.file){t.file->write(data,static_cast<std::streamsize>(length));if(!*t.file){t.failed=true;return 0;}}
        else t.body.append(data,length);
        t.count+=length;
        t.last_data=std::chrono::steady_clock::now();
        return length;
    }catch(...){t.failed=true;return 0;}
}
std::string size_text(std::uint64_t size) {
    if(size<1024)return std::to_string(size)+" bytes";
    if(size<1024*1024)return std::to_string(size/1024)+" KB";
    return std::to_string(size/(1024*1024))+" MB";
}
int transfer_progress(void* opaque,curl_off_t,curl_off_t,curl_off_t,curl_off_t) noexcept {
    auto& t=*static_cast<Transfer*>(opaque);
    try {
        if(t.cancel->load())return 1;
        const auto now=std::chrono::steady_clock::now();
        if(t.progress&&now-t.last_report>=std::chrono::seconds(2)) {
            const auto waited=std::chrono::duration_cast<std::chrono::seconds>(now-t.started).count();
            if(t.count==0)t.progress(t.phase+"，已等待 "+std::to_string(waited)+" 秒");
            else {
                auto message=t.phase+" "+size_text(t.count);
                if(t.expected)message+=" / "+size_text(t.expected)+" ("+std::to_string(t.count*100/t.expected)+"%)";
                if(now-t.last_data>=std::chrono::seconds(2))message+="，已等待 "+std::to_string(waited)+" 秒";
                t.progress(message);
            }
            t.last_report=now;
        }
        return 0;
    }catch(...) {t.failed=true;return 1;}
}
bool github_url(const std::string& url) {
    std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> parsed(curl_url(),curl_url_cleanup);
    if(!parsed||url.size()>8192||url.find_first_of("\r\n")!=std::string::npos||curl_url_set(parsed.get(),CURLUPART_URL,url.c_str(),0)!=CURLUE_OK)return false;
    const auto part=[&](CURLUPart key){char* raw=nullptr;const auto code=curl_url_get(parsed.get(),key,&raw,0);std::unique_ptr<char,decltype(&curl_free)> text(raw,curl_free);return code==CURLUE_OK?std::string(raw):std::string();};
    const auto host=part(CURLUPART_HOST),port=part(CURLUPART_PORT);
    return part(CURLUPART_SCHEME)=="https"&&(port.empty()||port=="443")&&part(CURLUPART_USER).empty()&&part(CURLUPART_PASSWORD).empty()&&
        (host=="api.github.com"||host=="github.com"||host=="release-assets.githubusercontent.com"||host=="objects.githubusercontent.com");
}
std::string download(std::string url,Transfer& transfer) {
    static const auto initialized=curl_global_init(CURL_GLOBAL_DEFAULT);require(initialized==CURLE_OK,"更新网络初始化失败");
    for(int redirects=0;redirects<=5;++redirects) {
        require(github_url(url),"升级下载地址不属于 GitHub HTTPS 服务");
        require(!transfer.cancel->load(),"更新已取消");
        std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);require(static_cast<bool>(curl),"无法连接 GitHub");
        const auto set=[&](CURLoption option,auto value){require(curl_easy_setopt(curl.get(),option,value)==CURLE_OK,"更新网络配置失败");};
        set(CURLOPT_URL,url.c_str());set(CURLOPT_USERAGENT,"KindleRSS-Updater/1");
        set(CURLOPT_FOLLOWLOCATION,0L);set(CURLOPT_PROTOCOLS_STR,"https");set(CURLOPT_CONNECTTIMEOUT,15L);
        set(CURLOPT_TIMEOUT,transfer.file?600L:35L);set(CURLOPT_LOW_SPEED_LIMIT,1024L);set(CURLOPT_LOW_SPEED_TIME,30L);
        set(CURLOPT_NOSIGNAL,1L);set(CURLOPT_SSL_VERIFYPEER,1L);set(CURLOPT_SSL_VERIFYHOST,2L);
        if(const auto ca=std::getenv("RSS_CA_BUNDLE");ca&&*ca)set(CURLOPT_CAINFO,ca);
        else if(fs::exists("/mnt/us/native-rss/certs/ca-bundle.pem"))set(CURLOPT_CAINFO,"/mnt/us/native-rss/certs/ca-bundle.pem");
        set(CURLOPT_WRITEFUNCTION,receive);set(CURLOPT_WRITEDATA,&transfer);
        set(CURLOPT_NOPROGRESS,0L);set(CURLOPT_XFERINFOFUNCTION,transfer_progress);set(CURLOPT_XFERINFODATA,&transfer);
        const auto result=curl_easy_perform(curl.get());
        require(!transfer.cancel->load(),"更新已取消");require(!transfer.failed,"更新下载超出大小限制或磁盘写入失败");
        require(result==CURLE_OK,std::string("连接 GitHub 失败：")+curl_easy_strerror(result));
        long status=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
        if(status==200)return transfer.body;
        require(status!=403&&status!=429,"GitHub 暂时限制请求，请稍后重试");
        if(status>=300&&status<400&&redirects<5) {
            char* next=nullptr;curl_easy_getinfo(curl.get(),CURLINFO_REDIRECT_URL,&next);
            require(next&&*next,"GitHub 返回无效跳转");url=next;
            // Redirect bodies must not become part of an archive or JSON document.
            transfer.count=0;transfer.body.clear();
            transfer.started=transfer.last_report=transfer.last_data=std::chrono::steady_clock::now();
            if(transfer.file){transfer.file->seekp(0);require(static_cast<bool>(*transfer.file),"无法重置下载文件");}
            continue;
        }
        fail(status==404?"当前没有可下载的正式版本":"GitHub 返回 HTTP "+std::to_string(status));
    }
    fail("GitHub 跳转次数过多");
}
std::string download_text(const std::string& url,const std::atomic<bool>& cancel,std::uint64_t limit,const UpdateProgress& progress,const std::string& phase) {
    Transfer transfer;transfer.limit=limit;transfer.cancel=&cancel;transfer.progress=progress;transfer.phase=phase;return download(url,transfer);
}
std::uint32_t little(const unsigned char* p,unsigned count) {std::uint32_t n=0;for(unsigned i=0;i<count;++i)n|=static_cast<std::uint32_t>(p[i])<<(i*8);return n;}
std::map<std::string,bool> extract(const fs::path& archive,const fs::path& destination,const std::atomic<bool>& cancel) {
    std::ifstream in(archive,std::ios::binary);require(static_cast<bool>(in),"无法打开更新包");
    std::map<std::string,bool> files;std::uint64_t total=0;
    for(;;) {
        std::array<unsigned char,30> header{};in.read(reinterpret_cast<char*>(header.data()),4);
        require(in.gcount()==4,"更新包被截断");const auto signature=little(header.data(),4);
        if(signature==0x02014b50)break;
        require(signature==0x04034b50&&files.size()<1024,"更新包 ZIP 格式无效");
        in.read(reinterpret_cast<char*>(header.data()+4),26);require(static_cast<bool>(in),"更新包被截断");
        const auto flags=little(header.data()+6,2),method=little(header.data()+8,2),crc=little(header.data()+14,4);
        const auto compressed=little(header.data()+18,4),size=little(header.data()+22,4),name_size=little(header.data()+26,2),extra=little(header.data()+28,2);
        require((flags&~0x800U)==0&&method==8&&name_size>0&&name_size<=240&&extra==0&&compressed<=package_limit&&size<=expanded_limit-total,"更新包 ZIP 特性不受支持");
        std::string name(name_size,'\0');in.read(name.data(),name_size);
        require(static_cast<bool>(in)&&allowed(name)&&files.emplace(name,false).second,"更新包含重复或越界路径");
        require(!cancel.load(),"更新已取消");total+=size;
        const auto path=destination/name;fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);require(static_cast<bool>(out),"无法解压更新包");
        z_stream stream{};require(inflateInit2(&stream,-MAX_WBITS)==Z_OK,"无法初始化更新包解压");
        std::array<unsigned char,65536> input{},output{};std::uint64_t produced=0;uLong actual_crc=crc32(0L,Z_NULL,0);int result=Z_OK;
        try {
            std::uint32_t remaining=compressed;
            while(remaining&&result!=Z_STREAM_END) {
                require(!cancel.load(),"更新已取消");const auto length=std::min<std::size_t>(remaining,input.size());
                in.read(reinterpret_cast<char*>(input.data()),static_cast<std::streamsize>(length));require(static_cast<bool>(in),"更新包被截断");remaining-=static_cast<std::uint32_t>(length);
                stream.next_in=input.data();stream.avail_in=static_cast<uInt>(length);
                do {
                    stream.next_out=output.data();stream.avail_out=output.size();result=inflate(&stream,Z_NO_FLUSH);
                    require(result==Z_OK||result==Z_STREAM_END,"更新包解压失败");const auto amount=output.size()-stream.avail_out;produced+=amount;
                    require(produced<=size,"更新包解压大小不符");out.write(reinterpret_cast<char*>(output.data()),static_cast<std::streamsize>(amount));
                    require(static_cast<bool>(out),"更新包解压写入失败");actual_crc=crc32(actual_crc,output.data(),static_cast<uInt>(amount));
                }while(result!=Z_STREAM_END&&(stream.avail_in||stream.avail_out==0));
            }
            require(result==Z_STREAM_END&&stream.avail_in==0&&remaining==0&&produced==size&&actual_crc==crc,"更新包文件校验失败");
        }catch(...){inflateEnd(&stream);throw;}
        inflateEnd(&stream);out.close();require(static_cast<bool>(out),"更新包写入失败");
        files[name]=name.rfind("native-rss/bin/",0)==0||(name.size()>3&&name.substr(name.size()-3)==".sh");
    }
    return files;
}
std::map<std::string,bool> validate_stage(const fs::path& folder,const std::string& version) {
    const auto stage=folder/"stage";
    safe_path(folder,"stage/native-rss/UPDATE.json");safe_path(folder,"stage/native-rss/MANIFEST.json");
    const auto metadata=query(read(stage/"native-rss/UPDATE.json"),"SELECT json_extract(?1,'$.version') WHERE json_valid(?1) AND json_type(?1,'$.format')='integer' AND json_extract(?1,'$.format')=1 AND json_extract(?1,'$.model')='Kindle'");
    require(metadata.size()==1&&metadata[0][0]==version,"此版本尚不支持在线升级，或安装包机型/版本不符");
    const auto rows=query(read(stage/"native-rss/MANIFEST.json"),"SELECT key,value,type FROM json_each(?1,'$.files')");
    std::map<std::string,bool> files;
    for(const auto& row:rows) {
        const auto& name=row[0];require(allowed(name)&&name!="native-rss/MANIFEST.json"&&row[2]=="text"&&digest_valid(row[1])&&files.emplace(name,false).second,"更新文件清单无效");
        safe_path(stage,name);require(fs::is_regular_file(stage/name)&&hash(stage/name)==row[1],"更新包逐文件校验失败");
        files[name]=name.rfind("native-rss/bin/",0)==0||(name.size()>3&&name.substr(name.size()-3)==".sh");
    }
    for(const auto* name:{"native-rss/bin/kindle-rss","native-rss/launch.sh","native-rss/UPDATE.json","native-rss/fonts/reader.ttf","native-rss/certs/ca-bundle.pem","extensions/native-rss/config.xml"})
        require(files.count(name),"更新包缺少必要文件");
    files["native-rss/MANIFEST.json"]=false;
    std::set<std::string> actual;
    for(const auto& item:fs::recursive_directory_iterator(stage)) {
        require(!item.is_symlink(),"升级暂存目录含符号链接");
        if(item.is_regular_file())actual.insert(item.path().lexically_relative(stage).generic_string());
        else require(item.is_directory(),"升级暂存目录含特殊文件");
    }
    require(actual.size()==files.size(),"更新包含清单外文件");for(const auto& name:actual)require(files.count(name),"更新包含清单外文件");
    return files;
}
void durable_copy(const fs::path& source,const fs::path& destination) {
    fs::create_directories(destination.parent_path());fs::copy_file(source,destination,fs::copy_options::overwrite_existing);
    fs::permissions(destination,fs::status(source).permissions());durable(destination);
}
struct Record {std::string path;bool existed;};
std::vector<Record> journal(const fs::path& folder) {
    std::istringstream lines(read(folder/"journal"));std::string line;std::vector<Record> records;std::set<std::string> paths;
    while(std::getline(lines,line)) {
        require(line.size()>2&&(line[0]=='0'||line[0]=='1')&&line[1]==' '&&allowed(line.substr(2))&&paths.insert(line.substr(2)).second,"升级恢复日志无效");
        records.push_back({line.substr(2),line[0]=='1'});require(records.size()<=1024,"升级恢复日志过大");
    }
    require(!records.empty(),"升级恢复日志为空");return records;
}
}
std::string app_version() {return "v" RSS_VERSION;}
UpdateRelease parse_update_release(const std::string& json,const std::string& current) {
    const auto rows=query(json,"SELECT json_extract(?1,'$.tag_name'),substr(coalesce(json_extract(?1,'$.body'),''),1,4096),json_type(?1,'$.prerelease') WHERE json_valid(?1) AND json_type(?1,'$.tag_name')='text' AND json_type(?1,'$.draft')='false' AND json_type(?1,'$.prerelease') IN ('true','false') AND json_type(?1,'$.assets')='array'");
    require(rows.size()==1,"GitHub 更新信息无效，或仍是草稿");
    UpdateRelease update;update.version=rows[0][0];update.notes=rows[0][1];
    const auto target=version_parts(update.version),installed=version_parts(current);
    const bool beta=target[3]!=1000000;
    require((rows[0][2]=="true")==beta&&(!beta||installed[3]!=1000000),"当前更新通道不接受此预发布版本");
    update.available=target>installed;
    if(update.notes.size()>=4096)update.notes+="\n更多内容请查看 GitHub Releases 的完整发布说明。";
    update.asset_name="RSS-Reader-"+update.version+"-Kindle.zip";
    const auto assets=query(json,"SELECT json_extract(value,'$.name'),json_extract(value,'$.browser_download_url'),json_extract(value,'$.size'),json_type(value,'$.size') FROM json_each(?1,'$.assets')");
    for(const auto& row:assets) {
        if(row[0]==update.asset_name) {
            require(update.asset_url.empty()&&row[1]==repository+update.version+"/"+update.asset_name&&row[3]=="integer","GitHub 安装包信息无效");
            update.asset_url=row[1];update.size=std::stoull(row[2]);require(update.size>0&&update.size<=package_limit,"安装包超过在线升级大小限制");
        }else if(row[0]=="SHA256SUMS.txt") {
            require(update.checksums_url.empty()&&row[1]==repository+update.version+"/SHA256SUMS.txt","GitHub 校验文件地址无效");update.checksums_url=row[1];
        }
    }
    require(!update.available||(!update.asset_url.empty()&&!update.checksums_url.empty()),"最新版缺少 Kindle 安装包或校验文件");return update;
}
UpdateRelease parse_update_releases(const std::string& json,const std::string& current) {
    version_parts(current);
    require(query(json,"SELECT 1 WHERE json_valid(?1) AND json_type(?1)='array'").size()==1,"GitHub 更新列表无效");
    UpdateRelease latest;latest.version=current;
    for(const auto& row:query(json,"SELECT value FROM json_each(?1) WHERE type='object'")) {
        UpdateRelease candidate;
        try {candidate=parse_update_release(row[0],current);}
        catch(const std::exception&) {continue;}
        if(candidate.available&&version_parts(candidate.version)>version_parts(latest.version))latest=std::move(candidate);
    }
    return latest;
}
UpdateRelease check_update(const std::atomic<bool>& cancel,const UpdateProgress& progress) {
    if(version_parts(app_version())[3]!=1000000)
        return parse_update_releases(download_text("https://api.github.com/repos/frevia/kindle-rss-reader/releases?per_page=100",cancel,2*1024*1024,progress,"正在检查更新"),app_version());
    return parse_update_release(download_text("https://api.github.com/repos/frevia/kindle-rss-reader/releases/latest",cancel,2*1024*1024,progress,"正在检查更新"),app_version());
}
void stage_update(const fs::path& archive,const fs::path& root,const std::string& version,const std::string& digest,const std::atomic<bool>& cancel) {
    const auto target=version_parts(version),current=version_parts(app_version());
    require((target[3]==1000000||current[3]!=1000000)&&target>current,"只能安装当前通道的新版本");
    require(digest_valid(digest)&&fs::file_size(archive)<=package_limit&&hash(archive)==digest,"安装包 SHA256 校验失败");
    const auto folder=update_folder(root);require(!fs::exists(folder/"journal"),"上次升级尚未恢复，请先重启阅读器");
    safe_path(root,"native-rss/.update/stage");fs::remove_all(folder/"stage");fs::create_directories(folder/"stage");
    fs::remove(folder/"ready");
    try {
        require(fs::space(root).available>expanded_limit+16*1024*1024,"空间不足，请至少保留 272 MB 可用空间");
        const auto extracted=extract(archive,folder/"stage",cancel);const auto validated=validate_stage(folder,version);
        for(const auto& item:validated)safe_path(root,item.first);
        require(extracted.size()==validated.size(),"安装包文件与清单不一致");require(!cancel.load(),"更新已取消");
        write(folder/"ready",version);::sync();
    }catch(...){fs::remove_all(folder/"stage");fs::remove(folder/"ready");throw;}
}
void prepare_update(const UpdateRelease& release,const fs::path& root,const std::atomic<bool>& cancel,const UpdateProgress& progress) {
    require(release.available&&release.asset_url==repository+release.version+"/"+release.asset_name&&release.checksums_url==repository+release.version+"/SHA256SUMS.txt","更新信息无效，请重新检查");
    require(!fs::exists("/var/local/native-rss/home/enabled"),"请先在 KUAL 停用首页 RSS，再升级阅读器");
    const auto folder=update_folder(root);require(!fs::exists(folder/"journal"),"上次升级尚未恢复");
    fs::create_directories(folder);fs::remove(folder/"ready");
    fs::remove_all(folder/"stage");
    require(fs::space(root).available>package_limit+expanded_limit+16*1024*1024,"空间不足，请至少保留 400 MB 可用空间");
    progress("正在获取 SHA256 校验文件");const auto checksums=download_text(release.checksums_url,cancel,65536,progress,"正在获取 SHA256 校验文件");
    std::istringstream lines(checksums);std::string line,digest;
    while(std::getline(lines,line))if(line.size()>66&&line.substr(66)==release.asset_name) {
        require(digest.empty()&&line.substr(64,2)=="  "&&digest_valid(line.substr(0,64)),"校验文件无效");digest=line.substr(0,64);
    }
    require(!digest.empty(),"发布校验文件缺少安装包");
    safe_path(root,"native-rss/.update/package.zip");std::ofstream out(folder/"package.zip",std::ios::binary|std::ios::trunc);
    require(static_cast<bool>(out),"无法创建更新下载文件");Transfer transfer;transfer.file=&out;transfer.limit=release.size;transfer.cancel=&cancel;transfer.progress=progress;transfer.phase="正在下载更新";transfer.expected=release.size;
    try {
        progress("正在下载更新，可随时取消");download(release.asset_url,transfer);out.close();
        require(static_cast<bool>(out)&&transfer.count==release.size&&fs::file_size(folder/"package.zip")==release.size,"安装包下载不完整");
        progress("正在校验并解压安装包");stage_update(folder/"package.zip",root,release.version,digest,cancel);
        safe_path(root,"native-rss/.update/runner");durable_copy(root/"native-rss/bin/kindle-rss",folder/"runner");
        fs::permissions(folder/"runner",fs::perms::owner_all);::sync();fs::remove(folder/"package.zip");
    }catch(...){out.close();fs::remove(folder/"package.zip");fs::remove(folder/"ready");throw;}
}
void backup_update_database(const fs::path& root,const UpdateProgress& progress) {
    const auto folder=update_folder(root);require(fs::exists(folder/"ready"),"更新尚未下载完成");
    safe_path(root,"native-rss/data/rss.sqlite");safe_path(root,"native-rss/.update/database.sqlite");
    sqlite3 *source=nullptr,*target=nullptr;
    const int opened=sqlite3_open_v2((root/"native-rss/data/rss.sqlite").c_str(),&source,SQLITE_OPEN_READONLY,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> from(source,sqlite3_close);
    require(opened==SQLITE_OK,"无法备份阅读数据库，升级已停止");fs::remove(folder/"database.sqlite");
    const int created=sqlite3_open((folder/"database.sqlite").c_str(),&target);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> to(target,sqlite3_close);require(created==SQLITE_OK,"无法创建升级前数据库备份");
    auto* backup=sqlite3_backup_init(to.get(),"main",from.get(),"main");require(backup!=nullptr,"无法备份阅读数据库");
    int copied=SQLITE_OK,finished=SQLITE_OK;auto last=std::chrono::steady_clock::now();
    try {
        do {
            copied=sqlite3_backup_step(backup,128);
            const auto now=std::chrono::steady_clock::now();
            if(progress&&(copied==SQLITE_DONE||now-last>=std::chrono::seconds(2))) {
                const int total=sqlite3_backup_pagecount(backup),remaining=sqlite3_backup_remaining(backup);
                progress("正在备份阅读数据 · "+std::to_string(total-remaining)+" / "+std::to_string(total)+" 页");last=now;
            }
        } while(copied==SQLITE_OK);
    } catch(...) {sqlite3_backup_finish(backup);throw;}
    finished=sqlite3_backup_finish(backup);
    require(copied==SQLITE_DONE&&finished==SQLITE_OK,"阅读数据库备份失败，升级已停止");to.reset();
    if(progress)progress("正在将阅读数据备份写入磁盘");durable(folder/"database.sqlite");
}
void apply_update(const fs::path& root,const UpdateProgress& progress) {
    require(!fs::exists("/var/local/native-rss/home/enabled"),"请先停用首页 RSS 再升级");
    const auto folder=update_folder(root);require(!fs::exists(folder/"journal"),"存在未完成的升级，请先恢复");
    if(progress)progress("正在验证待安装文件");
    const auto version=read(folder/"ready",32);const auto files=validate_stage(folder,version);
    require(version_parts(version)>version_parts(app_version()),"只能升级到新版本");
    safe_path(root,"native-rss/.update/backup");fs::remove_all(folder/"backup");fs::create_directories(folder/"backup");
    std::uint64_t needed=16*1024*1024;
    std::size_t done=0;
    for(const auto& item:files) {
        safe_path(root,item.first);if(fs::exists(root/item.first))needed+=fs::file_size(root/item.first);
        needed+=fs::file_size(folder/"stage"/item.first);
    }
    require(fs::space(root).available>needed,"空间不足，无法保留升级前程序");
    std::string log;
    for(const auto& item:files) {
        const auto path=root/item.first;require(!fs::exists(path)||fs::is_regular_file(path),"升级目标不是普通文件");const bool existed=fs::exists(path);
        if(existed)durable_copy(path,folder/"backup"/item.first);
        log+=(existed?"1 ":"0 ")+item.first+"\n";
        if(progress)progress("正在备份旧程序 "+std::to_string(++done)+" / "+std::to_string(files.size()));
    }
    safe_path(root,"native-rss/.update/journal.tmp");write(folder/"journal.tmp",log);fs::rename(folder/"journal.tmp",folder/"journal");::sync();
    try {
        done=0;
        for(const auto& item:files) {
            safe_path(root,item.first);const auto target=root/item.first;const auto temporary=fs::path(target.string()+".rss-update-new");
            safe_path(root,temporary.lexically_relative(root));durable_copy(folder/"stage"/item.first,temporary);
            fs::permissions(temporary,static_cast<fs::perms>(item.second?0755:0644));
            fs::rename(temporary,target);
            if(progress)progress("正在安装新程序 "+std::to_string(++done)+" / "+std::to_string(files.size()));
        }
        if(progress)progress("正在同步升级文件");
        ::sync();
        if(progress)progress("升级文件已同步");
    }catch(...){recover_update(root);throw;}
}
bool recover_update(const fs::path& root) {
    const auto folder=update_folder(root);if(!fs::exists(folder/"journal"))return false;
    const auto records=journal(folder);
    // Discard the staged update before rollback so a full disk still has room for restoration.
    fs::remove_all(folder/"stage");
    for(const auto& record:records) {
        safe_path(root,record.path);const auto target=root/record.path;
        const auto temporary=fs::path(target.string()+".rss-update-new");safe_path(root,temporary.lexically_relative(root));
        if(record.existed) {
            safe_path(folder/"backup",record.path);require(fs::is_regular_file(folder/"backup"/record.path),"升级备份缺失，请通过 USB 恢复");
            durable_copy(folder/"backup"/record.path,temporary);fs::rename(temporary,target);
        }else {fs::remove(temporary);fs::remove(target);}
    }
    if(fs::exists(folder/"database.sqlite")) {
        for(const auto* name:{"rss.sqlite","rss.sqlite.rss-update-new","rss.sqlite-wal","rss.sqlite-shm","rss.sqlite-journal"})safe_path(root,fs::path("native-rss/data")/name);
        durable_copy(folder/"database.sqlite",root/"native-rss/data/rss.sqlite.rss-update-new");
        // Old SQLite sidecars must not replay newer pages over the restored snapshot.
        for(const auto* name:{"rss.sqlite-wal","rss.sqlite-shm","rss.sqlite-journal"})fs::remove(root/"native-rss/data"/name);
        ::sync();
        fs::rename(root/"native-rss/data/rss.sqlite.rss-update-new",root/"native-rss/data/rss.sqlite");
    }
    ::sync();fs::remove(folder/"journal");fs::remove(folder/"ready");write(folder/"result","上次升级未完成，已恢复升级前的程序与阅读数据");::sync();return true;
}
void commit_update(const fs::path& root) {
    const auto folder=update_folder(root);if(!fs::exists(folder/"journal"))return;
    ::sync();fs::remove(folder/"journal");fs::remove(folder/"ready");write(folder/"result","在线升级完成 · "+app_version());::sync();
}
}
