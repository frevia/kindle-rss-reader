#pragma once

#include "model.hpp"
#include <string>
#include <string_view>

namespace rss {
Entry extract_fulltext(std::string_view html, const std::string& effective_url, const std::string& charset = "");
}
