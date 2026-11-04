#pragma once
#include <string>
#include <vector>

namespace rss {
struct QwenConfig {
    std::string api_key;
    std::string endpoint = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    std::string provider = "qwen";
    std::string model = "qwen-mt-flash";
};
QwenConfig qwen_config(const std::string& database_path);
void validate_qwen_config(const QwenConfig& config);
std::string qwen_request(const std::string& text, const std::vector<std::string>& protected_tokens = {},
                         const QwenConfig& config = {});
std::string qwen_translation(const std::string& response);
}
