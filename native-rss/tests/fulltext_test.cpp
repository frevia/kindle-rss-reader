#include "fulltext.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace rss;
    const std::string url = "https://example.test/posts/current";
    auto ok = [&](const std::string& html, const std::string& charset = "") {
        try{return extract_fulltext(html,url,charset);}catch(const std::exception&){std::cerr<<"Unexpected extraction failure: "<<html.substr(0,240)<<"\n";throw;}
    };
    auto rejects = [&](const std::string& html, const std::string& charset = "") {
        try { (void)extract_fulltext(html,url,charset); } catch (const std::runtime_error&) { return true; }
        return false;
    };

    const auto article = ok("<html><head><title>Story</title><meta name='author' content='Ada'></head><body><nav>Menu</nav><article><h1>Headline</h1><p>Short <em>real</em> article.</p><blockquote><p>A quote.</p></blockquote><pre><code>x &lt; y</code></pre></article><footer>Footer</footer></body></html>");
    assert(article.url == url && article.title == "Story" && article.author == "Ada");
    assert(article.html.find("<h1>Headline</h1>") != std::string::npos);
    assert(article.html.find("<em>real</em>") != std::string::npos);
    assert(article.html.find("<blockquote>") != std::string::npos && article.html.find("<pre><code>") != std::string::npos);
    assert(article.text.find("Short real article.") != std::string::npos && article.text.find("Footer") == std::string::npos);
    assert(article.images.empty());

    const auto div_post = ok("<div class='layout'><div class='menu'>tiny nav</div><div class='post'><p>Ordinary div based article body works.</p><p>Second paragraph stays.</p></div></div>");
    assert(div_post.text.find("Ordinary div based article body works.") != std::string::npos);
    assert(div_post.text.find("tiny nav") == std::string::npos);
    assert(div_post.text.find("Second paragraph stays.") != std::string::npos);
    const auto short_post = ok("<main><p>Hi.</p></main>");
    assert(short_post.text == "Hi.");
    const auto plain = ok("<div><p>中文正文 &amp; punctuation.</p></div>");
    assert(plain.text.find("中文正文 & punctuation.") != std::string::npos);
    const auto bare=ok("<body><p>A plain HTML essay can contain substantial prose without using a semantic article element.</p><p>This second paragraph continues the same essay and should be selected using the prose evidence.</p></body>");
    assert(bare.text.find("plain HTML essay")!=std::string::npos);
    const auto legacy=ok("<table><tr><td><a href='/nav'>Menu</a></td><td><font>Legacy essays use repeated line breaks instead of paragraphs, but the prose is still readable.<br><br>The next block continues the essay with substantial text and should survive HTML extraction.<br><br>The final block finishes the story without navigation.</font></td></tr></table>");
    assert(legacy.text.find("Legacy essays")!=std::string::npos&&legacy.text.find("finishes the story")!=std::string::npos);
    assert(rejects("<nav><div><p>This navigation must never be mistaken for the complete article.</p></div></nav>"));
    assert(rejects("<div hidden><article><p>This invisible text is not the readable story.</p></article></div>"));
    assert(rejects("<section id='comments'><div><p>Reader replies should not become the article body.</p></div></section>"));
    assert(rejects("<main><p><a href='/1'>First directory entry</a></p><p><a href='/2'>Second directory entry</a></p></main>"));
    const auto discussion=ok("<article class='commentary'><p>This article explains why CAPTCHA and 验证码 are sometimes difficult to read.</p><p>It is a real discussion of authentication, not a login screen.</p></article>");
    assert(discussion.text.find("real discussion")!=std::string::npos);

    const auto images = ok("<article><p>Photo story with enough prose.</p><figure><img data-src='../img/real.jpg' src='placeholder.gif' onerror='alert(1)' alt='A &amp; B'><figcaption>Caption.</figcaption></figure><img srcset='/wide.jpg 2x, /small.jpg 1x'></article>");
    assert(images.html.find("https://example.test/img/real.jpg") != std::string::npos);
    assert(images.html.find("placeholder.gif") == std::string::npos && images.html.find("onerror") == std::string::npos);
    assert(images.html.find("https://example.test/wide.jpg") != std::string::npos);
    assert(images.images.size() == 2 && images.images[0].url == "https://example.test/img/real.jpg");
    const auto base = ok("<base href='https://cdn.example.test/stories/'><article><p>Useful base URL test.</p><img src='photo.png'></article>");
    assert(base.images.size() == 1 && base.images[0].url == "https://cdn.example.test/stories/photo.png");

    const auto bad_links = ok("<article><p>Link scheme safety is checked.</p><a href='javascript:alert(1)' onclick='bad()'>safe text</a><img src='javascript:bad'><img src='data:image/png;base64,AA'></article>");
    assert(bad_links.html.find("javascript:") == std::string::npos && bad_links.html.find("onclick") == std::string::npos);
    assert(bad_links.text.find("safe text") != std::string::npos && bad_links.images.empty());
    const auto exclusions = ok("<article><p>Before block.</p><script>evil()</script><style>bad{}</style><iframe>frame words</iframe><object>obj</object><svg><text>svg words</text></svg><form>form words</form><p>After block.</p></article>");
    for (const char* omitted : {"evil", "frame words", "obj", "svg words", "form words", "bad{}"}) assert(exclusions.html.find(omitted) == std::string::npos);
    assert(exclusions.text.find("Before block.") != std::string::npos && exclusions.text.find("After block.") != std::string::npos);
    const auto preserve = ok("<main><p>Paragraph.</p><ul><li>One</li><li>Two</li></ul><table><tr><td>Cell</td></tr></table><strong>Bold</strong></main>");
    for (const char* tag : {"<p>", "<ul>", "<li>", "<table>", "<tr>", "<td>", "<strong>"}) assert(preserve.html.find(tag) != std::string::npos);
    const auto escaping = ok("<article><p>Escaped &amp; &lt;markup&gt; survives safely.</p></article>");
    assert(escaping.html.find("&amp;") != std::string::npos && escaping.html.find("&lt;markup&gt;") != std::string::npos);

    for (const auto* gate : {"<main><p>Sign in to continue to read this story.</p></main>", "<article><p>CAPTCHA: verify you are human.</p></article>", "<main><h1>Just a moment</h1><p>Checking your browser.</p></main>", "<article><p>登录后继续阅读，访问受限。</p></article>", "<main><p>请输入验证码以继续访问。</p></main>"}) assert(rejects(gate));
    for (const auto* invalid : {"", "<html><body><nav>Navigation only</nav><footer>Footer only</footer></body></html>", "<html><body><p>body text without an article container is rejected by design.</p></body></html>"}) assert(rejects(invalid));
    assert(!rejects("<article><p>UTF-8 is accepted, even when this post is short.</p></article>", "UTF-8"));
    assert(!rejects("<article><p>ASCII plain text.</p></article>", "US-ASCII"));
    assert(!rejects("<meta charset='windows-1252'><article><p>HTTP charset overrides an obsolete meta hint.</p></article>", "text/html; charset=UTF-8"));
    assert(!rejects("<meta http-equiv='Content-Type' content='text/html; charset=utf-8'><article><p>Meta declared UTF-8 is accepted.</p></article>"));
    assert(rejects("<meta charset='windows-1252'><article><p>Wrong declared encoding.</p></article>"));
    assert(rejects("<article><p>Unsupported HTTP charset.</p></article>", "Shift_JIS"));
    assert(rejects(std::string("<article><p>") + char(0xC3) + "</p></article>"));
    for(const auto malformed:{std::string("\xf5\x80\x80",3),std::string("\xf8\x80\x80",3),std::string("\xed\xa0\x80",3),std::string("\0",1)})
        assert(rejects("<article><p>Invalid character "+malformed+" in readable prose.</p></article>"));

    std::string deep = "<article>"; for (int i = 0; i < 70; ++i) deep += "<div>"; deep += "Deep content beyond allowed DOM depth."; for (int i = 0; i < 70; ++i) deep += "</div>"; deep += "</article>";
    assert(rejects(deep));
    std::string wide = "<article><p>Wide tree has useful prose.</p>"; for (int i = 0; i < 10020; ++i) wide += "<span>x</span>"; wide += "</article>";
    assert(rejects(wide));
    std::string too_large = "<article><p>" + std::string(520 * 1024, 'x') + "</p></article>";
    assert(rejects(too_large));
    assert(rejects("<article><p>"+std::string(150*1024,'&')+"</p></article>"));
    assert(rejects(std::string(8 * 1024 * 1024 + 1, 'x')));
    assert(rejects("<article><p>" + std::string(520 * 1024, 'x') + "</p></article>"));

    std::cout << "fulltext extractor checks passed\n";
}
