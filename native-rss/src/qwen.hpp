#pragma once
#include <string>

namespace rss {
struct QwenConfig {
    std::string api_key;
    std::string endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
};
QwenConfig qwen_config(const std::string& database_path);
void validate_qwen_config(const QwenConfig& config);
std::string qwen_request(const std::string& text);
std::string qwen_translation(const std::string& response);
}
