#include "cre_reader.hpp"
#include "feed.hpp"
#include "render.hpp"
#include "store.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    assert(rss::CreReader::available());
    const auto configured = std::getenv("RSS_FONT");
    const std::string font = configured ? configured : "/System/Library/Fonts/STHeiti Light.ttc";
    rss::Store store(":memory:");
    rss::Canvas canvas(1072, 1448, font);
    rss::Entry story;
    story.title = "受控正文与独立书签";
    story.html = "<h2>段落层级</h2><blockquote>这是一段引用。</blockquote><pre>  keep indentation\n    &lt;code&gt;</pre>";
    for (int i = 0; i < 140; ++i) story.html += "<p>第" + std::to_string(i) + "段：新版阅读位置与原排版位置分开保存，正文变更后旧书签不能错误定位到其他段落。</p>";
    story.text = rss::html_to_text(story.html);
    rss::CreReader reader(store, font);
    reader.open(story, "https://fixture.invalid/article", "来源与日期", 944, 1196, 34, 24);
    assert(reader.page_count() > 3);
    const auto anchor = reader.anchor(2);
    assert(reader.restore(anchor) == 2);
    reader.layout(880, 1196, 42, 24);
    const auto larger_page = reader.restore(anchor);
    assert(larger_page < reader.page_count());
    reader.draw(canvas, larger_page, 64, 148, 900);
    assert(std::any_of(canvas.pixels.begin(), canvas.pixels.begin() + 900 * 1072, [](auto pixel) { return pixel < 200; }));
    assert(std::all_of(canvas.pixels.begin() + 900 * 1072, canvas.pixels.end(), [](auto pixel) { return pixel == 255; }));
    rss::CreReader reopened(store, font);
    reopened.open(story, "https://fixture.invalid/article", "来源与日期", 944, 1196, 34, 24);
    assert(reopened.restore(anchor) == 2);
    assert(reopened.restore("invalid") == 0);
    auto changed = story;
    changed.html = "<p>新的正文。</p>";
    changed.text = rss::html_to_text(changed.html);
    reopened.open(changed, "https://fixture.invalid/article", "来源与日期", 944, 1196, 34, 24);
    assert(reopened.restore(anchor) == 0);
    auto legacy = story;legacy.html = "<p>以前的摘要。</p>";
    reopened.open(legacy, "https://fixture.invalid/article", "来源与日期", 944, 1196, 34, 24);
    assert(reopened.page_count() > 3); // A stale HTML field must not replace the stored full text.

    rss::Entry clean;
    clean.title = "资源边界";
    clean.html = "<p>保留正文 <strong>重点</strong><a>链接</a></p><img src='https://fixture.invalid/unknown' alt='未缓存'/>";
    auto hostile = clean;
    hostile.html = "<script>javascript text</script><style>@import url('file:///etc/passwd');</style>"
        "<iframe src='file:///etc/passwd'>hidden frame</iframe>"
        "<p style=\"background:url(file:///etc/passwd)\" onclick='bad()'>保留正文 <strong>重点</strong>"
        "<a href='javascript:bad()'>链接</a></p><img src='file:///etc/passwd' onerror='bad()' alt='未缓存'/>";
    rss::CreReader sanitized(store, font), expected(store, font);
    sanitized.open(hostile, "https://fixture.invalid/article", "metadata", 944, 1196, 34, 24);
    expected.open(clean, "https://fixture.invalid/article", "metadata", 944, 1196, 34, 24);
    assert(sanitized.anchor(0) == expected.anchor(0));
    canvas.clear();sanitized.draw(canvas, 0, 64, 148, 1344);
    const auto safe_pixels = canvas.pixels;
    canvas.clear();expected.draw(canvas, 0, 64, 148, 1344);
    assert(canvas.pixels == safe_pixels);

    rss::Feed feed;feed.url = "https://fixture.invalid/feed";
    const auto fid = store.add_feed(feed);
    rss::Entry illustrated;illustrated.guid = "cached-image";illustrated.title = "离线图片";
    illustrated.html = "<p>图片说明</p><figure><img src='/cached.png'/><figcaption>真实灰度缓存</figcaption></figure>";
    illustrated.text = rss::html_to_text(illustrated.html);
    rss::ArticleImage image;image.url = "https://fixture.invalid/cached.png";image.alt = "cached";
    illustrated.images.push_back(image);
    rss::ParsedFeed batch;batch.entries.push_back(illustrated);store.save_entries(fid, batch);
    illustrated = *store.entry(store.entries(rss::View::All).front().id);
    image.width = 100;image.height = 60;image.pixels.assign(6000, 77);
    store.save_image(illustrated.id, image);
    rss::CreReader cached(store, font);
    cached.open(illustrated, "https://fixture.invalid/article", "metadata", 944, 1196, 34, 24);
    canvas.clear();cached.draw(canvas, 0, 64, 148, 1344);
    assert(std::count(canvas.pixels.begin(), canvas.pixels.end(), 77) >= 5000);
    bool rejected = false;
    auto oversize = story;oversize.html.assign(512 * 1024 + 1, 'x');
    try { cached.open(oversize, "", "", 944, 1196, 34, 24); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && cached.page_count() > 0); // Failure retains the previous valid document.
    rejected = false;
    auto deep = story;deep.html.clear();
    for (int i = 0; i < 70; ++i) deep.html += "<div>";
    deep.html += "too deep";
    for (int i = 0; i < 70; ++i) deep.html += "</div>";
    deep.text = rss::html_to_text(deep.html);
    try { cached.open(deep, "", "", 944, 1196, 34, 24); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    setenv("RSS_CRE_CACHE", "/tmp", 1);
    rejected = false;
    try { cached.open(story, "", "", 944, 1196, 34, 24); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && cached.page_count() > 0);
    unsetenv("RSS_CRE_CACHE");
    setenv("RSS_UI_GUARD", "1", 1);
    rejected = false;
    try { cached.open(story, "", "", 944, 1196, 34, 24); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && cached.page_count() > 0);
    unsetenv("RSS_UI_GUARD");
    std::cout << "CREngine sanitization, cached resources, independent anchors and bounds passed\n";
}
