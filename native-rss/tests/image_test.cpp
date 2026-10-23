#include "image.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

int main(){
    const auto folder=std::filesystem::path(__FILE__).parent_path()/"fixtures";
    auto read=[&](const char* name){std::ifstream f(folder/name,std::ios::binary);assert(f);return std::string(std::istreambuf_iterator<char>(f),{});};
    rss::ArticleImage ref;ref.url="https://example.test/image";ref.alt="caption";
    const auto png=rss::decode_image(ref,read("colors.png"));
    assert(png.width==2&&png.height==2&&png.pixels.size()==4);
    assert(png.pixels[0]==77&&png.pixels[1]==149&&png.pixels[2]==29&&png.pixels[3]==255);
    const auto jpeg=rss::decode_image(ref,read("colors.jpg"));assert(jpeg.width==2&&jpeg.height==2);
    const auto webp=rss::decode_image(ref,read("colors.webp"));assert(webp.pixels==png.pixels);
    for(const auto& bytes:{std::string("broken image"),read("oversized.png"),read("inflate-limit.png"),read("colors.png").substr(0,30),read("oversized.webp"),read("colors.webp").substr(0,24)}){
        bool rejected=false;try{rss::decode_image(ref,bytes);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    // Failed decoding must release its allocation budget for subsequent images.
    assert(rss::decode_image(ref,read("colors.png")).pixels==png.pixels);
}
