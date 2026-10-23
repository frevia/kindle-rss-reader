#include "app.hpp"
#include "device.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>

template<class F> void measure(const char* name, int repetitions, F action) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repetitions; ++i) action();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << name << "," << std::fixed << std::setprecision(3) << ms / repetitions << "\n";
}
int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 3) throw std::runtime_error("usage: rss-performance FONT [ARTICLE_COUNT]");
        const int count = argc == 3 ? std::stoi(argv[2]) : 10000;
        if (count < 100 || count > 100000) throw std::runtime_error("article count must be 100..100000");
        rss::Store store(":memory:");
        rss::Feed feed; feed.url = "https://benchmark.invalid/rss"; feed.title = "性能测试"; feed.group = "测试/性能";
        const auto fid = store.add_feed(feed);
        rss::ParsedFeed parsed;
        for (int i = 0; i < count; ++i) {
            rss::Entry entry; entry.guid = std::to_string(i); entry.published = 1700000000 + i;
            entry.title = "性能测试：中文标题换行与阅读列表 Reader benchmark " + std::to_string(i);
            if (i % 3 == 0) entry.title += "，这是需要显示多行的较长文章标题，验证分页布局保持一致。";
            entry.text = "文章正文。"; parsed.entries.push_back(std::move(entry));
        }
        parsed.entries.back().text.clear();
        for (int i = 0; i < 1000; ++i) parsed.entries.back().text += "长文阅读性能测试，分页后继续阅读与保存位置。A paragraph of text.\n";
        store.save_entries(fid, parsed);
        parsed.entries.clear();
        if (store.entries(rss::View::All).size() != static_cast<std::size_t>(count))
            throw std::runtime_error("benchmark fixture count mismatch");
        std::cout << "articles," << count << "\noperation,mean_ms\n";
        measure("metadata_query", 3, [&] { store.entries(rss::View::All); });
        rss::Canvas canvas(1072, 1448, argv[1]);
        std::optional<rss::App> app;
        measure("startup_and_list", 1, [&] { app.emplace(store, canvas); app->draw(); });
        auto snapshot = [&](const char* name) {
            if (const auto folder = std::getenv("RSS_BENCH_SNAPSHOTS")) {
                std::filesystem::create_directories(folder);
                canvas.write_pgm((std::filesystem::path(folder) / name).string());
            }
        };
        measure("all_switch_and_draw", 3, [&] { app->show_view(rss::View::All); app->draw(); });
        snapshot("list.pgm");
        measure("navigation_first", 1, [&] { app->tap(100, 150); app->draw(); });
        snapshot("navigation.pgm");
        measure("navigation_redraw", 10, [&] { app->draw(); });
        app->back(); app->draw();
        measure("list_next_and_draw", 10, [&] { app->next(); app->draw(); });
        measure("list_previous_and_draw", 10, [&] { app->previous(); app->draw(); });
        measure("group_switch_and_draw", 3, [&] { app->show_group("测试"); app->draw(); });
        measure("search_and_draw", 1, [&] {
            app->tap(950, 150); app->text_input("性能"); app->submit(); app->draw();
        });
        rss::ArticleImage image; image.url = "https://benchmark.invalid/image";
        rss::Entry picture; picture.guid = "picture"; picture.title = "图片性能测试";
        picture.images.push_back(image);
        rss::ParsedFeed pictures; pictures.entries.push_back(picture); store.save_entries(fid, pictures);
        const auto picture_id = store.entries(rss::View::All, fid, picture.title).front().id;
        image.width = 944; image.height = 1000; image.pixels.assign(944 * 1000, 120);
        store.save_image(picture_id, image);
        measure("image_open_and_draw", 1, [&] {
            app->open(picture_id); app->draw(); app->tap(900, 50); app->draw();
        });
        measure("image_redraw", 10, [&] { app->draw(); });
        app->back(); app->back();
        const auto eid = store.entries(rss::View::All, fid, "Reader benchmark").front().id;
        measure("long_article_open_and_draw", 1, [&] { app->open(eid); app->draw(); });
        snapshot("reader.pgm");
        measure("reader_next_and_draw", 10, [&] { app->next(); app->draw(); });
        app->tap(530, 400);
        measure("reader_settings_draw", 1, [&] { app->draw(); });
        snapshot("settings.pgm");
        app->back();
        measure("return_to_list", 1, [&] { app->back(); app->draw(); });
        const auto previous = canvas.pixels;
        canvas.fill({20,20,100,100}, 0);
        measure("screen_diff", 10, [&] { rss::changed_region(previous, canvas.pixels); });
        measure("rss_icon", 100, [&] { canvas.icon(rss::Icon::Rss, {64,43,34,34}); });
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n"; return 1;
    }
}
