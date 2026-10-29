#include "home_patch.hpp"
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rss {
namespace {
constexpr std::size_t limit = 16 * 1024 * 1024;
struct File {
    int fd;
    explicit File(int value) : fd(value) { if (fd < 0) throw std::runtime_error("无法打开补丁文件"); }
    ~File() { close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
std::vector<unsigned char> read_file(const std::string& path, struct stat& info) {
    File file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fstat(file.fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > static_cast<off_t>(limit))
        throw std::runtime_error("补丁输入不是大小受限的普通文件");
    std::vector<unsigned char> data(static_cast<std::size_t>(info.st_size));
    std::size_t position = 0;
    while (position < data.size()) {
        const auto count = read(file.fd, data.data() + position, data.size() - position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("补丁输入读取失败");
        position += static_cast<std::size_t>(count);
    }
    unsigned char extra;
    if (read(file.fd, &extra, 1) != 0) throw std::runtime_error("补丁输入在读取时改变");
    return data;
}
std::array<unsigned char, 32> digest(const std::vector<unsigned char>& data) {
    std::array<unsigned char, 32> value{};
    unsigned int length = 0;
    if (!EVP_Digest(data.data(), data.size(), value.data(), &length, EVP_sha256(), nullptr) || length != value.size())
        throw std::runtime_error("SHA-256 校验失败");
    return value;
}
bool same_file(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
}

void apply_home_patch(const std::string& original, const std::string& recipe, const std::string& output) {
    struct stat source_info{}, recipe_info{}, output_info{};
    const auto source = read_file(original, source_info);
    const auto patch = read_file(recipe, recipe_info);
    if (lstat(output.c_str(), &output_info) == 0) {
        if (!S_ISREG(output_info.st_mode) || output_info.st_uid != geteuid() || output_info.st_nlink != 1 ||
            same_file(source_info, output_info) || same_file(recipe_info, output_info))
            throw std::runtime_error("输出路径不能覆盖原固件、补丁或符号链接");
    } else if (errno != ENOENT) throw std::runtime_error("无法检查补丁输出路径");
    if (patch.size() < 84 || std::memcmp(patch.data(), "RSSHBC1\n", 8))
        throw std::runtime_error("不支持的首页补丁格式");
    const auto source_hash = digest(source);
    if (std::memcmp(source_hash.data(), patch.data() + 8, 32))
        throw std::runtime_error("固件哈希不匹配；原文件未修改");
    std::size_t position = 72;
    const auto integer = [&]() -> std::uint32_t {
        if (position > patch.size() || patch.size() - position < 4) throw std::runtime_error("补丁被截断");
        std::uint32_t value = 0;
        for (unsigned int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(patch[position++]) << (i * 8);
        return value;
    };
    const auto source_size = integer(), output_size = integer(), count = integer();
    if (source_size != source.size() || output_size == 0 || output_size > limit || count > 100000)
        throw std::runtime_error("首页补丁边界无效");
    std::vector<unsigned char> result;
    result.reserve(output_size);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (position >= patch.size()) throw std::runtime_error("补丁被截断");
        const auto tag = patch[position++];
        if (tag == 0) {
            const auto offset = integer(), length = integer();
            if (offset > source.size() || length > source.size() - offset || length > output_size - result.size())
                throw std::runtime_error("补丁复制越界");
            result.insert(result.end(), source.begin() + offset, source.begin() + offset + length);
        } else if (tag == 1) {
            const auto length = integer();
            if (position > patch.size() || length > patch.size() - position || length > output_size - result.size())
                throw std::runtime_error("补丁数据越界");
            result.insert(result.end(), patch.begin() + position, patch.begin() + position + length);
            position += length;
        } else throw std::runtime_error("未知补丁操作");
    }
    if (position != patch.size() || result.size() != output_size) throw std::runtime_error("补丁输出长度不符");
    const auto result_hash = digest(result);
    if (std::memcmp(result_hash.data(), patch.data() + 40, 32)) throw std::runtime_error("补丁输出校验失败");
    const auto temporary = output + ".tmp." + std::to_string(getpid());
    bool created = false;
    try {
        File file(open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        created = true;
        std::size_t written = 0;
        while (written < result.size()) {
            const auto bytes = write(file.fd, result.data() + written, result.size() - written);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) throw std::runtime_error("无法写入首页补丁副本");
            written += static_cast<std::size_t>(bytes);
        }
        if (fsync(file.fd) || rename(temporary.c_str(), output.c_str()))
            throw std::runtime_error("无法原子保存首页补丁副本");
    } catch (...) { if (created) unlink(temporary.c_str()); throw; }
}
}
