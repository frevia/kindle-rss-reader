#include "feed.hpp"
#include "render.hpp"
#include <png.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxHtml = 8 * 1024 * 1024;
constexpr int kCanvasWidth = 1072, kCanvasHeight = 1448;
constexpr int kMarginX = 64, kMarginY = 84, kFontSize = 32, kLineHeight = 52;
constexpr int kContentWidth = kCanvasWidth - 2 * kMarginX;
constexpr int kContentHeight = 1280;
const std::string kFixtureBase = "https://fixture.invalid/";

std::string read_file(const fs::path& path, std::size_t limit) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("missing regular file: " + path.string());
    const auto size = fs::file_size(path);
    if (size == 0 || size > limit) throw std::runtime_error("file is empty or exceeds limit: " + path.string());
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file: " + path.string());
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in || static_cast<std::size_t>(in.gcount()) != data.size()) throw std::runtime_error("cannot read file: " + path.string());
    return data;
}

bool valid_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) continue;
        int extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
        if (extra < 0 || i + extra > s.size()) return false;
        std::uint32_t cp = c & ((1u << (6 - extra)) - 1);
        for (int n = 0; n < extra; ++n) {
            const auto b = static_cast<unsigned char>(s[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

rss::ArticleImage load_image(const fs::path& fixture_dir, const std::string& filename) {
    if (filename != "fixture-landscape.png" && filename != "fixture-portrait.png")
        throw std::runtime_error("unsupported image reference: " + filename);
    const auto base = fs::canonical(fixture_dir);
    const auto path = fs::canonical(base / filename);
    if (path.parent_path() != base) throw std::runtime_error("image path escapes fixture directory");
    const auto bytes = read_file(path, 8 * 1024 * 1024);
    rss::ArticleImage ref;
    ref.url = kFixtureBase + filename;
    ref.alt = filename;
    // The fixtures are already grayscale PNGs; avoid pulling all production codecs into this probe.
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size()))
        throw std::runtime_error("invalid fixture PNG");
    const bool landscape = filename == "fixture-landscape.png";
    if (png.width != (landscape ? 640u : 360u) || png.height != (landscape ? 360u : 640u)) {
        png_image_free(&png);
        throw std::runtime_error("unexpected fixture PNG dimensions");
    }
    png.format = PNG_FORMAT_GRAY;
    ref.width = png.width;
    ref.height = png.height;
    ref.pixels.resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, ref.pixels.data(), 0, nullptr)) {
        png_image_free(&png);
        throw std::runtime_error("fixture PNG decode failed");
    }
    png_image_free(&png);
    return ref;
}

void draw_image(rss::Canvas& canvas, const rss::ArticleImage& image, rss::Rect area) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() != static_cast<std::size_t>(image.width) * image.height)
        throw std::runtime_error("invalid decoded fixture image");
    const double scale = std::min({1.0, double(area.w) / image.width, double(area.h) / image.height});
    const int width = std::max(1, static_cast<int>(image.width * scale));
    const int height = std::max(1, static_cast<int>(image.height * scale));
    const int left = area.x + (area.w - width) / 2, top = area.y + (area.h - height) / 2;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        canvas.pixels[static_cast<std::size_t>(top + y) * canvas.width + left + x] =
            image.pixels[static_cast<std::size_t>(y * image.height / height) * image.width + x * image.width / width];
}

std::uint64_t peak_rss_kib() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024;
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#endif
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: baseline FONT HTML OUTDIR");
        const fs::path html_path = argv[2], output_dir = argv[3];
        const auto html = read_file(html_path, kMaxHtml);
        if (!valid_utf8(html)) throw std::runtime_error("HTML is not valid UTF-8");
        std::error_code ec;
        fs::create_directories(output_dir, ec);
        if (ec || !fs::is_directory(output_dir)) throw std::runtime_error("cannot create output directory");

        const auto layout_start = Clock::now();
        rss::Canvas canvas(kCanvasWidth, kCanvasHeight, argv[1]);
        const auto text = rss::html_to_text(html);
        if (text.empty() || !valid_utf8(text)) throw std::runtime_error("HTML produced empty or invalid UTF-8 text");
        const auto anchors = rss::html_image_anchors(html, kFixtureBase);
        std::vector<rss::ArticleImage> images;
        std::map<std::string, int> image_indices;
        std::vector<rss::InlineImage> inline_images;
        for (const auto& anchor : anchors) {
            if (anchor.url.rfind(kFixtureBase, 0) != 0) throw std::runtime_error("image reference is outside fixture URL");
            const auto filename = anchor.url.substr(kFixtureBase.size());
            auto found = image_indices.find(filename);
            if (found == image_indices.end()) {
                const int index = static_cast<int>(images.size());
                images.push_back(load_image(html_path.has_parent_path() ? html_path.parent_path() : fs::current_path(), filename));
                found = image_indices.emplace(filename, index).first;
            }
            const auto& image = images.at(found->second);
            inline_images.push_back({anchor.offset, anchor.caption_end, found->second, image.width, image.height});
        }
        const auto pages = canvas.paginate_article(text, inline_images, kFontSize, kLineHeight,
                                                   kContentWidth, kContentHeight, kContentHeight);
        const double layout_ms = std::chrono::duration<double, std::milli>(Clock::now() - layout_start).count();
        assert(!pages.empty());
        std::uint64_t output_size = 0;
        double render_total_ms = 0;
        std::size_t covered = 0;
        for (std::size_t p = 0; p < pages.size(); ++p) {
            const auto page_render_start = Clock::now();
            canvas.clear();
            bool has_content = false;
            for (const auto& item : pages[p].elements) {
                assert(item.rect.x >= 0 && item.rect.y >= 0 && item.rect.w >= 0 && item.rect.h >= 0);
                assert(item.rect.x + item.rect.w <= kContentWidth && item.rect.y + item.rect.h <= kContentHeight);
                const rss::Rect area{kMarginX + item.rect.x, kMarginY + item.rect.y, item.rect.w, item.rect.h};
                if (item.image >= 0) draw_image(canvas, images.at(static_cast<std::size_t>(item.image)), area);
                else {
                    assert(item.line.start == covered && item.line.end <= text.size());
                    covered = item.line.end;
                    canvas.text(area.x + (item.caption ? (area.w - canvas.measure(item.line.text, item.size)) / 2 : 0),
                                area.y + item.size, item.line.text, item.size, item.caption ? 90 : 20);
                }
                has_content = true;
            }
            assert(has_content);
            assert(canvas.pixels.size() == static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight);
            assert(std::any_of(canvas.pixels.begin(), canvas.pixels.end(), [](std::uint8_t pixel) { return pixel < 255; }));
            render_total_ms += std::chrono::duration<double, std::milli>(Clock::now() - page_render_start).count();
            std::ostringstream name;
            name << "page-" << std::setw(3) << std::setfill('0') << p + 1 << ".pgm";
            const auto path = output_dir / name.str();
            canvas.write_pgm(path.string());
            output_size += fs::file_size(path);
        }
        assert(covered == text.size());
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"engine\":\"baseline\",\"pages\":" << pages.size()
                  << ",\"layout_ms\":" << layout_ms << ",\"render_total_ms\":" << render_total_ms
                  << ",\"peak_rss_kib\":" << peak_rss_kib() << ",\"output_size\":" << output_size << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "baseline: " << e.what() << '\n';
        return 1;
    }
}
