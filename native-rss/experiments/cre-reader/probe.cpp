// Standalone experiment: renders local fixtures without a database or device access.
#include "lvdocview.h"
#include "lvfntman.h"
#include "hyphman.h"
#include "lvstsheet.h"
#include "lvimg.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int width = 1072, height = 1448;
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    std::string result;
    char buffer[8192];
    while (input) {
        input.read(buffer, sizeof(buffer));
        result.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (result.size() > 8 * 1024 * 1024) throw std::runtime_error("Input exceeds 8 MiB");
    }
    if (!input.eof()) throw std::runtime_error("Input read failed");
    return result;
}
void write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) throw std::runtime_error("Cannot write " + path.string());
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
long peak_rss() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}
struct Engine {
    explicit Engine(const char* font) {
        if (!InitFontManager(lString8::empty_str)) throw std::runtime_error("Font manager init failed");
        if (!fontMan->RegisterFont(lString8(font))) {
            ShutdownFontManager();
            throw std::runtime_error("Cannot register test font");
        }
        fontMan->RegularizeRegisteredFontsWeights();
        HyphMan::initDictionaries(lString32::empty_str);
    }
    ~Engine() { HyphMan::uninit(); ShutdownFontManager(); }
};
std::unique_ptr<LVDocView> open_document(const char* html, const std::string& css, int font_size) {
    auto view = std::make_unique<LVDocView>(8, true);
    view->Resize(width, height);
    view->setViewMode(DVM_PAGES, 1);
    view->setPageHeaderInfo(0);
    view->setPageMargins(lvRect(64, 84, 64, 84));
    view->setBackgroundColor(0xffffff);
    view->setTextColor(0x141414);
    lString32Collection faces;
    fontMan->getFaceList(faces);
    if (!faces.length()) throw std::runtime_error("Font has no faces");
    view->setDefaultFontFace(UnicodeToUtf8(faces[0]));
    view->setFontSize(font_size);
    view->setDefaultInterlineSpace(100); // Fixture CSS supplies the 162.5% line height.
    view->setStyleSheet(lString8(css.c_str()));
    if (!view->LoadDocument(html)) throw std::runtime_error("Cannot load test document");
    view->Render();
    if (view->getPageCount() < 1) throw std::runtime_error("Document has no pages");
    return view;
}
std::string frame(LVDocView& view) {
    LVGrayDrawBuf buffer(width, height, 8);
    buffer.Clear(0xffffff);
    view.Draw(buffer);
    std::string pixels(static_cast<std::size_t>(width) * height, '\xff');
    std::size_t ink = 0;
    for (int y = 0; y < height; ++y) {
        auto row = buffer.GetScanLine(y);
        std::copy_n(row, width, pixels.begin() + static_cast<std::size_t>(y) * width);
        for (int x = 0; x < width; ++x) ink += row[x] < 200;
    }
    assert(ink > 100);
    assert(pixels.size() == static_cast<std::size_t>(width) * height);
    return pixels;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::runtime_error("Usage: cre-probe FONT HTML CSS OUTDIR");
        const auto input = read_file(argv[2]);
        if (input.empty()) throw std::runtime_error("Empty HTML fixture");
        read_file(argv[3]);
        lString8 stylesheet;
        if (!LVLoadStylesheetFile(Utf8ToUnicode(argv[3]), stylesheet))
            throw std::runtime_error("Cannot load stylesheet and its import");
        const std::string css = stylesheet.c_str();
        const std::filesystem::path output(argv[4]);
        std::filesystem::create_directories(output);
        const auto start = Clock::now();
        Engine engine(argv[1]);
        auto view = open_document(argv[2], css, 32);
        const int pages = view->getPageCount();
        const double layout_ms = elapsed(start);
        double render_ms = 0;
        int image_count = 0;
        std::size_t output_size = 0;
        for (int page = 0; page < pages; ++page) {
            if (!view->goToPage(page)) throw std::runtime_error("Cannot navigate page");
            const auto render_start = Clock::now();
            const auto pixels = frame(*view);
            render_ms += elapsed(render_start);
            std::ostringstream name;
            name << "page-" << std::setfill('0') << std::setw(3) << page + 1 << ".pgm";
            const auto data = "P5\n1072 1448\n255\n" + pixels;
            write_file(output / name.str(), data);
            output_size += data.size();
        }
        // Page text ranges can exclude a standalone image at a boundary; verify the image objects instead.
        const auto count_images = [&](auto&& self, ldomNode* node) -> void {
            if (node->isElement() && node->getNodeName() == "img") {
                auto image = node->getObjectImageSource();
                assert(!image.isNull() && image->GetWidth() > 0 && image->GetHeight() > 0);
                ++image_count;
            }
            for (int i = 0; i < node->getChildCount(); ++i) self(self, node->getChildNode(i));
        };
        count_images(count_images, view->getDocument()->getRootNode());
        const int saved_page = std::min(1, pages - 1);
        const long memory = peak_rss();
        view->goToPage(saved_page);
        const auto bookmark = view->getBookmark().toString();
        assert(!bookmark.empty());
        write_file(output / "anchor.txt", UnicodeToUtf8(bookmark).c_str());
        const auto saved_anchor = Utf8ToUnicode(read_file(output / "anchor.txt").c_str());
        const auto original_text = view->getBookmark().getText();
        view.reset();
        view = open_document(argv[2], css, 32);
        auto restored = view->getDocument()->createXPointer(saved_anchor);
        assert(!restored.isNull());
        assert(restored.getText() == original_text);
        view->goToBookmark(restored);
        assert(view->getCurPage() == saved_page);
        view->setFontSize(40);
        view->Render();
        view->getPageCount();
        restored = view->getDocument()->createXPointer(saved_anchor);
        assert(!restored.isNull());
        assert(restored.getText() == original_text);
        view->goToBookmark(restored);
        assert(view->getCurPage() == view->getBookmarkPage(restored));
        bool hit_test = false;
        for (int y = 100; y < height - 84 && !hit_test; y += 32)
            for (int x = 80; x < width - 64 && !hit_test; x += 32) {
                auto point = view->getNodeByPoint(lvPoint(x, y), true, true);
                hit_test = !point.isNull() && !point.getText().empty();
            }
        assert(hit_test);
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"engine\":\"cre\",\"pages\":" << pages
                  << ",\"layout_ms\":" << layout_ms << ",\"render_total_ms\":" << render_ms
                  << ",\"peak_rss_kib\":" << memory << ",\"output_size\":" << output_size
                  << ",\"image_count\":" << image_count
                  << ",\"selfcheck_peak_rss_kib\":" << peak_rss()
                  << ",\"anchor_reopen\":true,\"anchor_reflow\":true,\"hit_test\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "cre-probe: " << error.what() << '\n';
        return 1;
    }
}
