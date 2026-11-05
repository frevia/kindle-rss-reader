// Run the real installer coordinator against disposable staged files, without device services.
#define main rss_command_main
#include "../src/main.cpp"
#undef main
#include <cassert>

int main(int argc,char** argv){
    assert(argc==3);const std::filesystem::path root=argv[1];const bool fail=std::string(argv[2])=="fail";
    rss::Canvas canvas(1072,1448,font_path());int frames=0;bool completed=false,recovering=false;
    try {
        install_update(root,[&](const std::string& message,int elapsed){
            if(frames++==0){
                assert(!std::filesystem::exists(root/"native-rss/.update/journal"));
                assert(read_file((root/"native-rss/bin/kindle-rss").string())=="old reader");
            }
            rss::draw_update_installation(canvas,message,elapsed);
            assert(elapsed>=0&&!canvas.pixels.empty());
            completed=message.find("安装完成")!=std::string::npos;
            recovering=message.find("正在恢复")!=std::string::npos;
        });
        assert(!fail&&completed&&frames>=2);
    }catch(const std::exception&){assert(fail&&recovering&&frames>=2);}
    if(fail)assert(read_file((root/"native-rss/bin/kindle-rss").string())=="old reader");
    else assert(read_file((root/"native-rss/bin/kindle-rss").string())=="new reader executable");
}
