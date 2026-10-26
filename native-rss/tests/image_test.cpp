#include "image.hpp"
#include <cassert>
#include <cmath>
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
    for(const auto* name:{"large-colors.png","large-colors.jpg","large-colors.webp","large-alpha.webp","large-10bit.avif"}){
        const auto large=rss::decode_image(ref,read(name));
        assert(large.width==944&&large.height>0&&large.height<=1130&&large.pixels.size()==static_cast<std::size_t>(large.width*large.height));
        assert(large.url==ref.url&&large.alt==ref.alt&&large.error.empty());
        for(const auto sample:{std::pair<int,int>{118,77},{354,149},{590,29},{826,255}})
            assert(std::abs(int(large.pixels[sample.first])-sample.second)<=4);
    }
    const auto portrait=rss::decode_image(ref,read("large-portrait.webp"));
    assert(portrait.width==35&&portrait.height==1130);
    for(auto value:portrait.pixels)assert(std::abs(int(value)-96)<=2);
    const auto avif=rss::decode_image(ref,read("colors-alpha.avif"));
    assert(avif.width==16&&avif.height==16);
    assert(std::abs(int(avif.pixels[0])-77)<=2&&std::abs(int(avif.pixels[8])-149)<=2);
    assert(std::abs(int(avif.pixels[8*16])-29)<=2&&avif.pixels[8*16+8]==255);
    for(const auto* name:{"oversized.png","oversized.webp"}){
        const auto formerly_blocked=rss::decode_image(ref,read(name));
        assert(formerly_blocked.width==944&&formerly_blocked.height==1);
    }
    auto oversized_avif=read("large-10bit.avif");const auto ispe=oversized_avif.find("ispe");assert(ispe!=std::string::npos);
    for(auto offset:{ispe+8,ispe+12}){oversized_avif[offset]=0;oversized_avif[offset+1]=0;oversized_avif[offset+2]=0x30;oversized_avif[offset+3]=0;}
    auto oversized_lossless=read("oversized.webp");
    assert(oversized_lossless.substr(12,4)=="VP8L");
    const unsigned dimensions=4095u|(4095u<<14);for(int i=0;i<4;++i)oversized_lossless[21+i]=static_cast<char>(dimensions>>(8*i));
    bool limited=false;try{rss::decode_image(ref,oversized_lossless);}catch(const std::runtime_error& e){limited=std::string(e.what()).find("安全解码上限")!=std::string::npos;}assert(limited);
    for(const auto& bytes:{std::string("broken image"),read("inflate-limit.png"),read("colors.png").substr(0,30),read("colors.webp").substr(0,24),read("large-10bit.avif").substr(0,64),oversized_avif}){
        bool rejected=false;try{rss::decode_image(ref,bytes);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    // Failed decoding must release its allocation budget for subsequent images.
    assert(rss::decode_image(ref,read("colors.png")).pixels==png.pixels);
    assert(rss::decode_image(ref,read("colors-alpha.avif")).pixels==avif.pixels);
}
