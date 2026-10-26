#include "feed.hpp"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    using namespace rss;
    const auto f = parse_feed(R"xml(<?xml version="1.0"?>
      <rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/">
        <channel><title>News &amp; Notes</title><item><title>Story</title>
          <guid>urn:story:1</guid><link>../story/1</link>
          <description>short</description><content:encoded><![CDATA[<p>Hello <b>world</b></p><script>bad()</script>]]></content:encoded>
          <pubDate>Tue, 02 Jan 2024 12:00:00 +0200</pubDate></item><item/></channel>
      </rss>)xml", "https://example.test/feeds/daily.xml");
    assert(f.title == "News & Notes");
    assert(f.entries.size() == 1);
    assert(f.entries[0].url == "https://example.test/story/1");
    assert(f.entries[0].html.find("<b>world</b>") != std::string::npos);
    assert(f.entries[0].text.find("Hello world") != std::string::npos);
    assert(f.entries[0].text.find("bad") == std::string::npos);
    assert(f.entries[0].published == 1704189600); // 10:00 UTC

    const auto atom = parse_feed(R"xml(<feed xmlns="http://www.w3.org/2005/Atom" xml:base="https://example.test/base/">
      <title>Atom</title><entry><id>tag:one</id><title>Entry</title><link href="item"/>
      <content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>Atom <em>markup</em></p></div></content>
      <updated>2024-01-02T12:00:00+02:00</updated></entry></feed>)xml", "https://example.test/feed");
    assert(atom.entries.size() == 1);
    assert(atom.entries[0].url == "https://example.test/base/item");
    assert(atom.entries[0].html.find("<em>markup</em>") != std::string::npos);
    assert(atom.entries[0].published == 1704189600);
    const auto escaped_atom = parse_feed(R"xml(<feed xmlns="http://www.w3.org/2005/Atom"><entry>
      <id>html</id><content type="html">&lt;p&gt;中文 &amp;amp; 正文&lt;/p&gt;</content>
      <updated>2024-01-02T12:00:00+0230</updated></entry></feed>)xml", "https://example.test/feed");
    assert(escaped_atom.entries[0].text == "中文 & 正文");
    assert(escaped_atom.entries[0].published == 1704187800);
    const auto plain_atom = parse_feed(R"xml(<feed xmlns="http://www.w3.org/2005/Atom"><entry>
      <id>plain</id><content type="text">Use &lt;tag&gt; literally &amp; safely.</content></entry></feed>)xml", "https://example.test/feed");
    assert(plain_atom.entries[0].text == "Use <tag> literally & safely.");
    const auto bad_date = parse_feed("<rss><channel><item><title>x</title><pubDate>Tue, 02 Jan 2024 12:00:00 +</pubDate></item></channel></rss>", "https://example.test/feed");
    assert(bad_date.entries[0].published == 0);
    for (const auto* invalid : {"<html><body>Not a feed</body></html>", "<rss>", ""}) {
        bool invalid_rejected = false;
        try { parse_feed(invalid, "https://example.test/feed"); } catch (const std::runtime_error&) { invalid_rejected = true; }
        assert(invalid_rejected);
    }

    assert(html_to_text("<p>A &amp; B</p><pre>x\n y</pre>").find("A & B") != std::string::npos);
    const auto opml = parse_opml(R"xml(<opml version="2.0"><body><outline text="Tech"><outline text="One &amp; More" title="One &amp; More" type="rss" xmlUrl="https://a.test/feed?x=1&amp;y=2"/><outline text="Nested"><outline text="Two" xmlUrl="http://b.test/rss"/></outline></outline><outline text="Dup" xmlUrl="https://a.test/feed?x=1&amp;y=2"/><outline text="Bad" xmlUrl="file:///tmp/no"/></body></opml>)xml");
    assert(opml.size() == 2);
    assert(opml[0].group == "Tech");
    assert(opml[1].group == "Tech/Nested");
    const auto roundtrip = parse_opml(export_opml(opml));
    assert(roundtrip.size() == 2 && roundtrip[0].url == opml[0].url);
    assert(roundtrip[0].title == opml[0].title);
    assert(roundtrip[0].group == opml[0].group);
    std::vector<std::string> empty_groups;
    const auto directory_only=parse_opml(R"xml(<opml><body><outline text="Empty &amp; Root"><outline text="Nested"/></outline></body></opml>)xml",&empty_groups);
    assert(directory_only.empty());
    assert((empty_groups==std::vector<std::string>{"Empty & Root","Empty & Root/Nested"}));
    const auto with_empty=parse_opml(export_opml(opml,empty_groups),&empty_groups);
    assert(with_empty.size()==opml.size());
    assert(std::find(empty_groups.begin(),empty_groups.end(),"Empty & Root/Nested")!=empty_groups.end());
    assert(export_opml({},empty_groups).find("Empty &amp; Root")!=std::string::npos);

    bool rejected = false;
    try { parse_feed("<!DOCTYPE x [<!ENTITY ext SYSTEM 'file:///etc/passwd'>]><rss><channel><title>&ext;</title></channel></rss>", "https://x.test/"); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    const auto images=html_images("<img src='../one.png' alt='图 &amp; 文'><img data-src='//cdn.test/two.jpg' src='placeholder.png'><img src='../one.png'><img src='file:///etc/passwd'><img src='data:image/png;base64,AA'>","https://example.test/posts/story");
    assert(images.size()==2&&images[0].url=="https://example.test/one.png"&&images[0].alt=="图 & 文");
    assert(images[1].url=="https://cdn.test/two.jpg");
    std::string many;for(int i=0;i<30;++i)many+="<img src='/"+std::to_string(i)+".png'>";
    assert(html_images(many,"https://example.test").size()==20);
    const auto anchored_html = "  <p>前文 <img src='/same.png'><img src='/same.png'> 后文</p>  <figure><img data-src='../caption.png' src='bad'><figcaption>图注</figcaption></figure><script><img src='/hidden.png'></script><img src='javascript:bad'>";
    const auto anchored_text = html_to_text(anchored_html);
    const auto anchors = html_image_anchors(anchored_html, "https://example.test/base/");
    assert(anchors.size() == 3);
    assert(anchors[0].url == "https://example.test/same.png" && anchors[1].url == anchors[0].url);
    assert(anchors[0].offset == anchored_text.find("前文") + std::string("前文 ").size());
    assert(anchors[1].offset >= anchors[0].offset);
    assert(anchors[2].url == "https://example.test/caption.png");
    assert(anchors[2].caption_end > anchors[2].offset);
    assert(anchored_text.substr(anchors[2].offset, anchors[2].caption_end - anchors[2].offset).find("图注") != std::string::npos);
    for (const auto& anchor : anchors) assert(anchor.offset <= anchor.caption_end && anchor.caption_end <= anchored_text.size());
    const auto normalized = html_to_text("  <p>中文  <img src='/x'>\t后文</p> <p>尾</p>  ");
    const auto normalized_anchors = html_image_anchors("  <p>中文  <img src='/x'>\t后文</p> <p>尾</p>  ", "https://example.test/");
    assert(normalized_anchors.size() == 1 && normalized_anchors[0].offset == normalized.find("后文"));
    assert(normalized_anchors[0].offset <= normalized.size());
    const auto wrapped=html_image_anchors("<figure><a><img src='/x'>正文</a><figcaption>图注</figcaption></figure>","https://example.test/");
    assert(wrapped.size()==1&&wrapped[0].caption_end==wrapped[0].offset);
    const auto nested=html_image_anchors("<figure><img src='/x'><figcaption>图注<img src='/y'>尾</figcaption></figure>","https://example.test/");
    assert(nested.size()==2&&nested[0].caption_end<=nested[1].offset);
    std::string repeats;for(int i=0;i<250;++i)repeats+="<img src='/x'>";
    assert(html_image_anchors(repeats,"https://example.test/").size()==200);
    std::cout << "feed parser checks passed\n";
}
