#include "translation.hpp"
#include "feed.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

using namespace rss;

static Entry entry(std::string html) {
    Entry e; e.title="A title"; e.html=std::move(html); e.text=html_to_text(e.html);
    e.url="https://example.test/article";
    return e;
}
template<class F> static bool throws(const F& f) { try { f(); } catch (const std::runtime_error&) { return true; } return false; }

int main() {
    auto context=entry("<p>Outside of the <a href='https://example.test/earth'>Earth's atmosphere</a>, the Moon appears <a href='https://example.test/photo'>magnificent</a>ly gray.</p>");
    const auto context_doc=translation_document(context);
    assert(context_doc.blocks[1].request=="Outside of the Earth's atmosphere, the Moon appears magnificently gray.");
    auto e=entry("<p>Hello <strong>world</strong> <a href='https://example.test/'>link</a>.</p><p>第二段</p>");
    auto d=translation_document(e);
    assert(d.blocks.size()==3 && !d.blocks[0].request.empty());
    assert(d.blocks[1].request=="Hello world link.");
    assert(d.revision.rfind("rss-translation-v3:",0)==0);
    assert(translation_units("A \xC2\xA0中\n") == 2);
    std::vector<std::string> translations{"标题", "你好，世界。这里是链接。", "第二段译文"};
    auto translated=translated_entry(e,d,translations,ReadingMode::Translated);
    auto bilingual=translated_entry(e,d,translations,ReadingMode::Bilingual);
    assert(translated.title=="标题" && translated.text==html_to_text(translated.html));
    assert(translated.html.find("你好，世界。这里是链接。")!=std::string::npos);
    assert(translated.html.find("href=")==std::string::npos);
    assert(bilingual.html.find("href=\"https://example.test/\"")!=std::string::npos);
    assert(bilingual.html.find("rss-pair-break")!=std::string::npos);
    assert(bilingual.text.find("link.\n")!=std::string::npos);
    assert(bilingual.html.find("class=\"rss-translation\"")!=std::string::npos);
    assert(bilingual.title=="A title\n标题");
    auto original=translated_entry(e,d,{},ReadingMode::Original);
    assert(original.title==e.title && original.html==e.html && original.text==e.text);
    assert(original.reading_mode==ReadingMode::Original);
    assert(original.reading_anchors.size()==2 && original.reading_anchors[0].source==0 && original.reading_anchors[0].display==0);
    assert(original.reading_anchors[1].source==d.blocks[1].source_units && original.reading_anchors[1].display==d.blocks[1].source_units);
    auto web=e;web.web_content=true;assert(translation_revision(e)==translation_revision(web));

    auto rich=entry("<p>Tom &amp; Jo <a href='https://example.test/?a=1&amp;b=2'>A &lt; B</a> <code>keep_me()</code> after</p>");
    const auto rich_doc=translation_document(rich);
    std::vector<std::string> sent;
    const auto rich_response=translate_block(rich_doc.blocks[1],[&](const std::string& input) {
        sent.push_back(input);
        assert(input.find("href")==std::string::npos && input.find("Tom & Jo A < B")!=std::string::npos);
        assert(input.find("keep_me()")==std::string::npos);
        return "汤姆与乔，甲 < 乙 & 丙，执行 "+rich_doc.blocks[1].protected_markup[0].first+" 之后";
    });
    assert(sent.size()==1 && sent[0]==rich_doc.blocks[1].request);
    const auto restored=validate_translation(rich_doc.blocks[1],rich_response);
    assert(restored.find("甲 &lt; 乙 &amp; 丙")!=std::string::npos);
    assert(restored.find("href=")==std::string::npos);
    assert(restored.find("<code>keep_me()</code>")!=std::string::npos);
    assert(restored.find(" 之后")!=std::string::npos);
    for(const auto& invalid:{std::string("<script>bad()</script>"),std::string("<a href='https://evil.test'>中文</a>"),std::string(""),std::string("\xc0\xaf")})
        assert(throws([&]{translate_block(rich_doc.blocks[1],[&](const auto&){return invalid;});}));
    assert(throws([&]{translate_block(rich_doc.blocks[1],[&](const auto&)->std::string{throw std::runtime_error("cancelled");});}));
    assert(rich.html.find("Tom &amp; Jo")!=std::string::npos);
    TranslationBlock punctuation;punctuation.html=punctuation.request=" 123, .!? ";
    assert(translate_block(punctuation,[](const auto&)->std::string{assert(false);return "";})==punctuation.request);

    auto nested=entry("<ul><li>outer<ul><li>inner</li></ul></li></ul><table><tr><td>cell</td><td><em>value</em></td></tr></table>");
    auto nd=translation_document(nested);
    assert(nd.blocks.size()==5);
    std::vector<std::string> nt(nd.blocks.size()); nt[0]="";for(std::size_t i=1;i<nt.size();++i)nt[i]=nd.blocks[i].request;
    auto nr=translated_entry(nested,nd,nt,ReadingMode::Bilingual);
    assert(nr.html.find("<ul>")!=std::string::npos && nr.html.find("<table>")!=std::string::npos);

    auto protected_entry=entry("<p>Caption</p><p>Run <code>x &lt; y</code> now</p><figure><img src='https://example.test/image.png' alt='photo'/><figcaption>Caption text</figcaption></figure><pre><code>if (x &lt; y) {}</code></pre>");
    protected_entry.images.push_back({"https://example.test/image.png","photo",{},0,0,{}}); protected_entry.text=html_to_text(protected_entry.html);
    auto pd=translation_document(protected_entry);
    bool image=false,code=false,pre=false,inline_code=false;std::size_t inline_index=0;for(std::size_t i=1;i<pd.blocks.size();++i){for(auto& p:pd.blocks[i].protected_markup){image|=p.second.find("<img")!=std::string::npos;code|=p.second.find("<code")!=std::string::npos;pre|=p.second.find("<pre")!=std::string::npos;if(p.second.find("<code")!=std::string::npos&&pd.blocks[i].request.find("Run")!=std::string::npos){inline_code=true;inline_index=i;}}assert(pd.blocks[i].request.find("https://example.test/image.png")==std::string::npos);}
    assert(image&&code&&pre);
    assert(inline_code);
    auto code_response=pd.blocks[inline_index].request;
    code_response.replace(code_response.find("Run"),3,"执行");
    assert(validate_translation(pd.blocks[inline_index],code_response).find("<code")!=std::string::npos);
    auto duplicated=pd.blocks[inline_index].request+" "+pd.blocks[inline_index].protected_markup[0].first;
    assert(throws([&]{validate_translation(pd.blocks[inline_index],duplicated);}));
    auto pt=std::vector<std::string>(pd.blocks.size());pt[0]="";for(std::size_t i=1;i<pt.size();++i)pt[i]=pd.blocks[i].request;
    pt[inline_index]=code_response;
    auto pr=translated_entry(protected_entry,pd,pt,ReadingMode::Bilingual);
    assert(pr.html.find("https://example.test/image.png")!=std::string::npos);
    assert(pr.html.find("<img")!=std::string::npos && pr.html.find("<img",pr.html.find("<img")+1)==std::string::npos);
    assert(pr.html.find("<pre")!=std::string::npos && pr.html.find("<pre",pr.html.find("<pre")+1)==std::string::npos);
    assert(pr.html.find("执行")!=std::string::npos);

    auto linked_image=entry("<p>See <a href='https://example.test/photo'><strong>the photo<img src='https://example.test/image.png'/></strong></a> here.</p>"
                            "<p><a href='https://example.test/second'><img src='https://example.test/image.png'/> Another photo</a></p>");
    linked_image.images=html_images(linked_image.html,linked_image.url);
    const auto image_doc=translation_document(linked_image);
    std::vector<std::string> image_translations;
    for(const auto& block:image_doc.blocks)image_translations.push_back(block.request);
    for(const auto mode:{ReadingMode::Original,ReadingMode::Translated,ReadingMode::Bilingual}) {
        const auto rendered=translated_entry(linked_image,image_doc,image_translations,mode);
        std::size_t count=0,at=0;while((at=rendered.html.find("<img",at))!=std::string::npos){++count;at+=4;}
        assert(count==2);
        if(mode!=ReadingMode::Translated)assert(rendered.html.find("https://example.test/photo")!=std::string::npos);
        assert(rendered.text.find("Another photo")!=std::string::npos);
        if(mode==ReadingMode::Original)assert(rendered.html==linked_image.html);
    }

    Entry fallback=entry("<script>bad</script><p>wrong</p>");fallback.text="第一行\nSecond line";auto fd=translation_document(fallback);
    assert(fd.blocks.size()==3 && fd.html.find("wrong")==std::string::npos);
    auto missing=std::vector<std::string>(fd.blocks.size());missing[0]="";auto mr=translated_entry(fallback,fd,missing,ReadingMode::Translated);
    assert(mr.html.find("本段未翻译")!=std::string::npos);
    assert(throws([&]{validate_translation(d.blocks[1],"<script>alert(1)</script>");}));
    assert(validate_translation(d.blocks[1],"你好，完整的一段译文。")=="你好，完整的一段译文。");
    assert(throws([&]{validate_translation(d.blocks[2],"</div><script>bad()</script>");}));
    assert(throws([&]{validate_translation(d.blocks[2],"<p onclick='bad()'>中文</p>");}));
    assert(throws([&]{validate_translation(d.blocks[2],"RSSKEEP123X9Q");}));
    assert(validate_translation(d.blocks[2],"RSSKEEP 是一个名称")=="RSSKEEP 是一个名称");
    assert(validate_translation(d.blocks[2],"<5 & 3>1")=="&lt;5 &amp; 3&gt;1");
    assert(throws([&]{validate_translation(d.blocks[2],std::string("\xc0\xaf"));}));
    assert(throws([&]{auto bad=d.blocks[1];bad.protected_markup={{"RSSPfakeX0Q","<code>x</code>"}};validate_translation(bad,"RSSPfakeX0Q RSSPfakeX0Q");}));

    auto long_text=std::string(7000,'a')+"。"+std::string(500,'b');auto long_entry=entry("<p>"+long_text+"</p>");auto ld=translation_document(long_entry);
    assert(ld.blocks.size()>2);for(std::size_t i=1;i<ld.blocks.size();++i)assert(ld.blocks[i].request.size()<=6000);
    std::string expanded;for(int i=0;i<5000;++i)expanded+="中";
    assert(validate_translation(d.blocks[2],expanded)==expanded);
    auto long_title=e;long_title.title=std::string(16000,'a');const auto title_doc=translation_document(long_title);
    assert(title_doc.title_blocks>1);
    for(std::size_t i=0;i<title_doc.title_blocks;++i)assert(title_doc.blocks[i].request.size()<=6000);
    auto unicode=entry("<p>"+expanded+"</p>");const auto unicode_doc=translation_document(unicode);
    std::string rebuilt;for(std::size_t i=unicode_doc.title_blocks;i<unicode_doc.blocks.size();++i)rebuilt+=html_to_text(unicode_doc.blocks[i].html);
    assert(rebuilt==expanded);
    assert(translation_revision(e)!=translation_revision(entry("<p>changed</p>")));
    Entry too_big=e;too_big.text=std::string(512*1024+1,'x');assert(throws([&]{translation_document(too_big);}));
    Entry deep=e;deep.html.clear();for(int i=0;i<66;++i)deep.html+="<div>";deep.html+="x";for(int i=0;i<66;++i)deep.html+="</div>";deep.text=html_to_text(deep.html);assert(throws([&]{translation_document(deep);}));
}
