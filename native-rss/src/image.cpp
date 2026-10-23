#include "image.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <cstdlib>
#include <cstddef>
#include <webp/decode.h>
namespace {
constexpr std::size_t decode_budget=32*1024*1024;
thread_local std::size_t decode_allocated=0;
struct alignas(std::max_align_t) Allocation {std::size_t size;};
void* image_alloc(std::size_t size){
    if(size>decode_budget-decode_allocated)return nullptr;
    auto* block=static_cast<Allocation*>(std::malloc(sizeof(Allocation)+size));if(!block)return nullptr;
    block->size=size;decode_allocated+=size;return block+1;
}
void image_free(void* pointer){
    if(!pointer)return;auto* block=static_cast<Allocation*>(pointer)-1;
    decode_allocated-=block->size;std::free(block);
}
void* image_realloc(void* pointer,std::size_t size){
    if(!pointer)return image_alloc(size);
    auto* block=static_cast<Allocation*>(pointer)-1;const auto old=block->size;
    if(size>decode_budget-(decode_allocated-old))return nullptr;
    block=static_cast<Allocation*>(std::realloc(block,sizeof(Allocation)+size));if(!block)return nullptr;
    block->size=size;decode_allocated=decode_allocated-old+size;return block+1;
}
}
#define STBI_MALLOC(size) image_alloc(size)
#define STBI_REALLOC(pointer,size) image_realloc(pointer,size)
#define STBI_FREE(pointer) image_free(pointer)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include "../third_party/stb/stb_image.h"

namespace rss {
ArticleImage decode_image(const ArticleImage& reference,std::string_view bytes){
    if(bytes.empty()||bytes.size()>8*1024*1024)throw std::runtime_error("图片为空或超过 8 MB");
    const auto* raw=reinterpret_cast<const stbi_uc*>(bytes.data());
    int width=0,height=0,channels=0;
    const bool webp=bytes.size()>=12&&bytes.substr(0,4)=="RIFF"&&bytes.substr(8,4)=="WEBP";
    WebPBitstreamFeatures features{};
    if(webp){
        if(WebPGetFeatures(raw,bytes.size(),&features)!=VP8_STATUS_OK)throw std::runtime_error("WebP 图片损坏");
        if(features.has_animation)throw std::runtime_error("暂不支持动态 WebP 图片");
        width=features.width;height=features.height;
    }else if(!stbi_info_from_memory(raw,static_cast<int>(bytes.size()),&width,&height,&channels))
        throw std::runtime_error("图片格式不支持或文件损坏（支持 JPEG / PNG / WebP）");
    if(width<=0||height<=0||width>2048||height>2048)throw std::runtime_error("图片边长超过 2048 像素限制");
    std::vector<stbi_uc> webp_pixels;
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> rgba(nullptr,stbi_image_free);
    if(webp){
        webp_pixels.resize(static_cast<std::size_t>(width)*height*4);
        if(!WebPDecodeRGBAInto(raw,bytes.size(),webp_pixels.data(),webp_pixels.size(),width*4))throw std::runtime_error("WebP 图片解码失败");
    }else{
        rgba.reset(stbi_load_from_memory(raw,static_cast<int>(bytes.size()),&width,&height,&channels,4));
        if(!rgba)throw std::runtime_error("图片损坏或解码内存不足");
    }
    const auto* pixels=webp?webp_pixels.data():rgba.get();
    ArticleImage result;result.url=reference.url;result.alt=reference.alt;
    const double scale=std::min({1.0,944.0/width,1130.0/height});
    result.width=std::max(1,static_cast<int>(width*scale));result.height=std::max(1,static_cast<int>(height*scale));
    result.pixels.resize(result.width*result.height);
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x){
        const int left=x*width/result.width,right=(x+1)*width/result.width;
        const int top=y*height/result.height,bottom=(y+1)*height/result.height;
        std::uint64_t sum=0,count=0;
        for(int sy=top;sy<bottom;++sy)for(int sx=left;sx<right;++sx){
            const auto* p=pixels+(sy*width+sx)*4;
            const unsigned gray=(77u*p[0]+150u*p[1]+29u*p[2]+128)/256;
            sum+=(gray*p[3]+255u*(255-p[3])+127)/255;++count;
        }
        result.pixels[y*result.width+x]=static_cast<std::uint8_t>(sum/count);
    }
    return result;
}
}
