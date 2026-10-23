#pragma once
#include "model.hpp"
#include <string_view>
namespace rss {
ArticleImage decode_image(const ArticleImage& reference,std::string_view bytes);
}
