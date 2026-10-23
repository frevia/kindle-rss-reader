#include "pinyin.hpp"
#include <algorithm>
#include <iterator>
#include <string_view>

namespace rss {
namespace {
struct PinyinEntry { const char* key; const char* text; };
#include "../third_party/pinyin/table.h"
}
std::vector<std::string> pinyin_candidates(const std::string& spelling){
    if(spelling.empty()||spelling.size()>32||spelling.find_first_not_of("abcdefghijklmnopqrstuvwxyz")!=std::string::npos)return {};
    auto first=std::lower_bound(std::begin(pinyin_entries),std::end(pinyin_entries),spelling,
        [](const PinyinEntry& entry,const std::string& key){return std::string_view(entry.key)<key;});
    std::vector<std::string> result;
    for(;first!=std::end(pinyin_entries)&&first->key==spelling;++first)result.emplace_back(first->text);
    return result;
}
}
