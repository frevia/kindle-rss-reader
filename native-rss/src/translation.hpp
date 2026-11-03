#pragma once

#include "model.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rss {
struct TranslationBlock {
    std::string html, request;
    std::vector<std::pair<std::string,std::string>> protected_markup;
    std::size_t source_units = 0;
};
struct TranslationDocument {
    std::string revision, html;
    std::vector<TranslationBlock> blocks;
    std::size_t title_blocks = 1;
};
std::string translation_revision(const Entry&);
std::size_t translation_units(std::string_view);
TranslationDocument translation_document(const Entry&);
std::string translate_block(const TranslationBlock&, const std::function<std::string(const std::string&)>&);
std::string validate_translation(const TranslationBlock&, const std::string&);
Entry translated_entry(const Entry&, const TranslationDocument&, const std::vector<std::string>& translations, ReadingMode);
}
