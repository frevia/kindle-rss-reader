#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace rss {
// Direct argv execution, no shell; nonzero exits and deadlines raise an error.
void run_bounded(const std::vector<std::string>& args, std::chrono::milliseconds deadline);
// Must run before acquiring device resources or starting threads.
void bind_to_launcher(const std::string& parent_pid);
}
