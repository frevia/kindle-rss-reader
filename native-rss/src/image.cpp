#include "image.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <cstdlib>
#include <cstddef>
#include <utility>
#include <webp/decode.h>
#include <avif/avif.h>
namespace {
// PNG holds inflated scanlines and RGBA pixels together, even for a 4.5MP screenshot.
constexpr std::size_t decode_budget=64*1024*1024;
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
#define STBI_MAX_DIMENSIONS 16384
#include "../third_party/stb/stb_image.h"

namespace rss {
ArticleImage decode_image(const ArticleImage& reference,std::string_view bytes){
    if(bytes.empty()||bytes.size()>8*1024*1024)throw std::runtime_error("图片为空或超过 8 MB");
    const auto* raw=reinterpret_cast<const stbi_uc*>(bytes.data());
    int width=0,height=0,channels=0;
    const auto fit=[](int w,int h,std::uint64_t limit){
        if(w<=0||h<=0||w>16384||h>16384||static_cast<std::uint64_t>(w)*h>limit)
            throw std::runtime_error("图片分辨率超过安全解码上限");
        const double scale=std::min({1.0,944.0/w,1130.0/h});
        return std::pair<int,int>{std::max(1,static_cast<int>(w*scale)),std::max(1,static_cast<int>(h*scale))};
    };
    const bool webp=bytes.size()>=12&&bytes.substr(0,4)=="RIFF"&&bytes.substr(8,4)=="WEBP";
    const avifROData avif_data{raw,bytes.size()};
    std::vector<stbi_uc> decoded;
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> rgba(nullptr,stbi_image_free);
    if(webp){
        WebPDecoderConfig config{};
        if(!WebPInitDecoderConfig(&config)||WebPGetFeatures(raw,bytes.size(),&config.input)!=VP8_STATUS_OK)
            throw std::runtime_error("WebP 图片损坏");
        if(config.input.has_animation)throw std::runtime_error("暂不支持动态 WebP 图片");
        // Lossless WebP may need the full source buffer even when output scaling is enabled.
        const auto size=fit(config.input.width,config.input.height,(config.input.format==2?8ull:64ull)*1024*1024);
        width=size.first;height=size.second;decoded.resize(static_cast<std::size_t>(width)*height*4);
        std::unique_ptr<WebPDecBuffer,decltype(&WebPFreeDecBuffer)> output(&config.output,WebPFreeDecBuffer);
        config.options.use_scaling=width!=config.input.width||height!=config.input.height;
        config.options.scaled_width=width;config.options.scaled_height=height;
        config.output.colorspace=MODE_RGBA;config.output.is_external_memory=1;
        config.output.u.RGBA.rgba=decoded.data();config.output.u.RGBA.stride=width*4;config.output.u.RGBA.size=decoded.size();
        if(WebPDecode(raw,bytes.size(),&config)!=VP8_STATUS_OK)throw std::runtime_error("WebP 图片损坏或解码内存不足");
    }else if(avifPeekCompatibleFileType(&avif_data)){
        std::unique_ptr<avifDecoder,decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(),avifDecoderDestroy);
        if(!decoder)throw std::runtime_error("AVIF 解码内存不足");
        decoder->maxThreads=1;decoder->imageSizeLimit=8*1024*1024;decoder->imageDimensionLimit=16384;decoder->imageCountLimit=1;
        if(avifDecoderSetIOMemory(decoder.get(),raw,bytes.size())!=AVIF_RESULT_OK||avifDecoderParse(decoder.get())!=AVIF_RESULT_OK)
            throw std::runtime_error("AVIF 图片损坏或分辨率超过安全解码上限");
        const auto size=fit(decoder->image->width,decoder->image->height,8ull*1024*1024);
        if(avifDecoderNextImage(decoder.get())!=AVIF_RESULT_OK)throw std::runtime_error("AVIF 图片损坏或解码内存不足");
        width=size.first;height=size.second;
        if(avifImageScale(decoder->image,width,height,&decoder->diag)!=AVIF_RESULT_OK)
            throw std::runtime_error("AVIF 图片缩放失败");
        decoded.resize(static_cast<std::size_t>(width)*height*4);
        avifRGBImage rgb{};avifRGBImageSetDefaults(&rgb,decoder->image);
        rgb.depth=8;rgb.format=AVIF_RGB_FORMAT_RGBA;rgb.alphaPremultiplied=AVIF_FALSE;rgb.pixels=decoded.data();rgb.rowBytes=width*4;
        if(avifImageYUVToRGB(decoder->image,&rgb)!=AVIF_RESULT_OK)throw std::runtime_error("AVIF 图片颜色转换失败");
    }else{
        if(!stbi_info_from_memory(raw,static_cast<int>(bytes.size()),&width,&height,&channels))
            throw std::runtime_error("图片格式不支持或文件损坏（支持 JPEG / PNG / WebP / AVIF）");
        fit(width,height,8ull*1024*1024);
        rgba.reset(stbi_load_from_memory(raw,static_cast<int>(bytes.size()),&width,&height,&channels,4));
        if(!rgba)throw std::runtime_error("图片损坏或解码内存不足");
    }
    const auto* pixels=decoded.empty()?rgba.get():decoded.data();
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
