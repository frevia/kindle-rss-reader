#include "pinyin.hpp"
#include <algorithm>
#include <cassert>

int main(){
    const auto contains=[](const auto& rows,const auto& text){return std::find(rows.begin(),rows.end(),text)!=rows.end();};
    const auto ni=rss::pinyin_candidates("ni");assert(!ni.empty()&&ni.front()=="你");
    assert(contains(rss::pinyin_candidates("keji"),"科技"));
    assert(contains(rss::pinyin_candidates("xinwen"),"新闻"));
    assert(contains(rss::pinyin_candidates("lv"),"绿"));
    assert(rss::pinyin_candidates("shi").size()>6);
    for(const auto& key:{"", "zzzz", "NI", "ni hao", "你", "ni\n"})assert(rss::pinyin_candidates(key).empty());
    assert(rss::pinyin_candidates(std::string(10000,'a')).empty());
}
