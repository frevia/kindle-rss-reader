#include "translation.hpp"
#include "feed.hpp"
#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace rss {
namespace {
constexpr std::size_t max_input=512*1024,max_output=2*1024*1024,max_request=6000;
const std::set<std::string> allowed{"p","div","section","article","main","h1","h2","h3","h4","h5","h6","blockquote","pre","code","strong","b","em","i","u","s","del","small","sup","sub","ul","ol","li","figure","figcaption","table","thead","tbody","tr","td","th","a","br","hr","img"};
const std::set<std::string> hidden{"head","script","style","iframe","object","embed","svg","math","video","audio","form","input","textarea","template","noscript","link","meta","base"};
const std::set<std::string> blocks{"p","div","section","article","main","h1","h2","h3","h4","h5","h6","blockquote","pre","ul","ol","li","figure","figcaption","table","thead","tbody","tr","td","th","hr"};
using Document=std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>;

std::string escape(std::string_view value) {
    std::string out;
    for(char c:value) {
        if(c=='&')out+="&amp;";else if(c=='<')out+="&lt;";else if(c=='>')out+="&gt;";
        else if(c=='"')out+="&quot;";else out+=c;
    }
    return out;
}
std::string property(xmlNode* node,const char* key) {
    std::unique_ptr<xmlChar,decltype(xmlFree)> value(xmlGetProp(node,BAD_CAST key),xmlFree);
    return value?reinterpret_cast<const char*>(value.get()):"";
}
std::size_t utf8_length(std::string_view text,std::size_t at) {
    const auto c=static_cast<unsigned char>(text[at]);
    const std::size_t size=c<128?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
    if(!size||size>text.size()-at)throw std::runtime_error("翻译文本包含无效 UTF-8");
    for(std::size_t i=1;i<size;++i)if((static_cast<unsigned char>(text[at+i])&0xc0)!=0x80)throw std::runtime_error("翻译文本包含无效 UTF-8");
    if((size==3&&((c==0xe0&&static_cast<unsigned char>(text[at+1])<0xa0)||(c==0xed&&static_cast<unsigned char>(text[at+1])>=0xa0)))||
       (size==4&&((c==0xf0&&static_cast<unsigned char>(text[at+1])<0x90)||(c==0xf4&&static_cast<unsigned char>(text[at+1])>=0x90)))||c==0)
        throw std::runtime_error("翻译文本包含无效 UTF-8");
    return size;
}
void bounds(const Entry& entry) {
    if(entry.html.size()>max_input||entry.text.size()>max_input||entry.title.size()>16384)throw std::runtime_error("文章超出翻译输入大小限制");
    for(const auto* text:{&entry.html,&entry.text,&entry.title})for(std::size_t i=0;i<text->size();)i+=utf8_length(*text,i);
}
Document parse(const std::string& html) {
    Document doc(htmlReadMemory(html.data(),static_cast<int>(html.size()),nullptr,"UTF-8",
        HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_COMPACT),xmlFreeDoc);
    if(!doc)throw std::runtime_error("翻译文档无法解析");
    return doc;
}
std::string fingerprint(std::string_view text) {
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:text){hash^=c;hash*=1099511628211ULL;}
    return std::to_string(hash); // Content identity only, never an authorization boundary.
}
std::string attributes(xmlNode* node,bool strict) {
    std::string out;
    for(auto attr=node->properties;attr;attr=attr->next) {
        const std::string key(reinterpret_cast<const char*>(attr->name));
        if(key!="href"||!xmlStrEqual(node->name,BAD_CAST "a")) {
            if(strict)throw std::runtime_error("翻译结果包含无效属性");
            continue;
        }
        const auto value=property(node,"href");
        if(value.rfind("https://",0)&&value.rfind("http://",0)&&value.rfind("mailto:",0)) {
            if(strict)throw std::runtime_error("翻译结果包含无效链接");
            continue;
        }
        out+=" href=\""+escape(value)+"\"";
    }
    return out;
}
std::string canonical(xmlNode* start,const Entry& entry,bool strict,std::size_t& count,int depth=0) {
    std::string out;
    for(auto node=start;node;node=node->next) {
        if(depth>64||++count>10000)throw std::runtime_error("翻译文档结构超过限制");
        if(node->type==XML_TEXT_NODE||node->type==XML_CDATA_SECTION_NODE) {
            if(node->content)out+=escape(reinterpret_cast<const char*>(node->content));
        }else if(node->type==XML_ELEMENT_NODE) {
            const std::string tag(reinterpret_cast<const char*>(node->name));
            if(hidden.count(tag)){if(strict)throw std::runtime_error("翻译结果包含危险内容");continue;}
            if(tag=="html"||tag=="body"||!allowed.count(tag)) {
                if(strict)throw std::runtime_error("翻译结果包含无效标签");
                out+=canonical(node->children,entry,strict,count,depth+1);continue;
            }
            if(tag=="img") {
                if(strict)throw std::runtime_error("翻译结果包含未授权图片");
                auto raw=property(node,"data-src");if(raw.empty())raw=property(node,"src");
                std::unique_ptr<xmlChar,decltype(xmlFree)> uri(xmlBuildURI(BAD_CAST raw.c_str(),BAD_CAST entry.url.c_str()),xmlFree);
                const std::string url=uri?reinterpret_cast<const char*>(uri.get()):raw;
                if(std::any_of(entry.images.begin(),entry.images.end(),[&](const ArticleImage& image){return image.url==url;}))
                    out+="<img src=\""+escape(url)+"\" alt=\""+escape(property(node,"alt"))+"\"/>";
                continue;
            }
            out+="<"+tag+attributes(node,strict);
            if(tag=="br"||tag=="hr")out+="/>";
            else out+=">"+canonical(node->children,entry,strict,count,depth+1)+"</"+tag+">";
        }
        if(out.size()>max_output)throw std::runtime_error("翻译文档超过大小限制");
    }
    return out;
}
struct Fragment {
    Document doc;
    xmlNode* root=nullptr;
    explicit Fragment(const std::string& html):doc(parse("<html><body><div id=\"rss-fragment\">"+html+"</div></body></html>")) {
        const auto find=[&](auto&& self,xmlNode* start)->void {
            for(auto node=start;node;node=node->next) {
                if(node->type==XML_ELEMENT_NODE&&xmlStrEqual(node->name,BAD_CAST "div")&&property(node,"id")=="rss-fragment") {
                    if(root)throw std::runtime_error("翻译文档边界无效");root=node;
                }else self(self,node->children);
            }
        };
        find(find,xmlDocGetRootElement(doc.get()));
        if(!root)throw std::runtime_error("翻译文档边界无效");
        const auto outside=[&](auto&& self,xmlNode* start)->void {
            for(auto node=start;node;node=node->next) {
                if(node==root)continue;
                if(node->type==XML_TEXT_NODE) {
                    if(node->content&&translation_units(reinterpret_cast<const char*>(node->content)))throw std::runtime_error("翻译内容越过段落边界");
                }else if(node->type==XML_ELEMENT_NODE) {
                    const std::string tag(reinterpret_cast<const char*>(node->name));
                    if(tag!="html"&&tag!="body"&&tag!="head")throw std::runtime_error("翻译内容越过段落边界");
                    self(self,node->children);
                }
            }
        };
        outside(outside,xmlDocGetRootElement(doc.get()));
    }
};
std::string dump(xmlNode* start) {
    std::unique_ptr<xmlBuffer,decltype(&xmlBufferFree)> buffer(xmlBufferCreate(),xmlBufferFree);
    if(!buffer)throw std::bad_alloc();
    for(auto node=start;node;node=node->next)if(xmlNodeDump(buffer.get(),node->doc,node,0,0)<0)throw std::runtime_error("翻译文档生成失败");
    return std::string(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),xmlBufferLength(buffer.get()));
}
std::string without_images(const std::string& html) {
    if(html.find("<img")==std::string::npos)return html;
    Fragment fragment(html);
    const auto remove=[&](auto&& self,xmlNode* node)->void {
        while(node) {
            auto next=node->next;
            if(node->type==XML_ELEMENT_NODE&&xmlStrEqual(node->name,BAD_CAST "img")) {
                xmlUnlinkNode(node);xmlFreeNode(node);
            }else self(self,node->children);
            node=next;
        }
    };
    remove(remove,fragment.root->children);return dump(fragment.root->children);
}
std::string signature(const std::string& html) {
    Fragment fragment(html);std::size_t count=0;
    const auto inspect=[&](auto&& self,xmlNode* start,int depth)->std::string {
        std::string out;
        for(auto node=start;node;node=node->next) {
            if(depth>64||++count>10000)throw std::runtime_error("翻译结果结构超过限制");
            if(node->type!=XML_ELEMENT_NODE)continue;
            const std::string tag(reinterpret_cast<const char*>(node->name));
            if(!allowed.count(tag)||hidden.count(tag)||tag=="img"||tag=="code"||tag=="pre")throw std::runtime_error("翻译结果包含无效标签");
            out+="<"+tag+attributes(node,true)+">"+self(self,node->children,depth+1)+"</"+tag+">";
        }
        return out;
    };
    return inspect(inspect,fragment.root->children,0);
}
TranslationBlock make_block(const std::string& html,std::size_t serial) {
    TranslationBlock block;block.html=html;block.source_units=translation_units(html_to_text(html));
    if(html.empty())return block;
    Fragment fragment(html);
    auto salt=fingerprint(html+std::to_string(serial));
    while(html.find("RSSKEEP"+salt)!=std::string::npos)salt+='X';
    const auto protect=[&](auto&& self,xmlNode* node)->void {
        while(node) {
            auto next=node->next;
            if(node->type==XML_ELEMENT_NODE) {
                const std::string tag(reinterpret_cast<const char*>(node->name));
                if(tag=="img"||tag=="pre"||tag=="code") {
                    std::unique_ptr<xmlBuffer,decltype(&xmlBufferFree)> buffer(xmlBufferCreate(),xmlBufferFree);
                    if(!buffer)throw std::bad_alloc();
                    xmlNodeDump(buffer.get(),node->doc,node,0,0);
                    const std::string markup(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),xmlBufferLength(buffer.get()));
                    const auto token="RSSKEEP"+salt+"X"+std::to_string(block.protected_markup.size())+"Q";
                    block.protected_markup.emplace_back(token,markup);
                    auto replacement=xmlNewText(BAD_CAST token.c_str());if(!replacement)throw std::bad_alloc();
                    xmlReplaceNode(node,replacement);xmlFreeNode(node);
                }else self(self,node->children);
            }
            node=next;
        }
    };
    protect(protect,fragment.root->children);block.request=dump(fragment.root->children);
    auto visible=block.request;
    for(const auto& item:block.protected_markup)visible.erase(visible.find(item.first),item.first.size());
    if(translation_units(html_to_text(visible))==0)block.request.clear();
    return block;
}
std::vector<std::string> split_html(const std::string& raw) {
    std::vector<std::string> parts;std::vector<std::pair<std::string,std::string>> stack;std::string current;
    const auto close=[&]{std::string text;for(auto it=stack.rbegin();it!=stack.rend();++it)text+="</"+it->first+">";return text;};
    const auto flush=[&]{parts.push_back(current+close());current.clear();for(const auto& item:stack)current+=item.second;};
    for(std::size_t i=0;i<raw.size();) {
        std::size_t end=i+utf8_length(raw,i);std::string name;bool closing=false,tag=false,empty=false;
        if(raw[i]=='<') {
            end=raw.find('>',i);if(end==std::string::npos)throw std::runtime_error("翻译段落标签不完整");++end;tag=true;
            closing=raw[i+1]=='/';const auto begin=i+(closing?2:1);auto until=begin;
            while(until<end&&std::isalnum(static_cast<unsigned char>(raw[until])))++until;
            name=raw.substr(begin,until-begin);empty=raw[end-2]=='/'||name=="br"||name=="hr"||name=="img";
        }else if(raw[i]=='&') {
            end=raw.find(';',i);if(end==std::string::npos)throw std::runtime_error("翻译段落实体不完整");++end;
        }
        const auto piece=raw.substr(i,end-i);
        if(current.size()+piece.size()+close().size()+32>max_request&&!current.empty())flush();
        current+=piece;
        if(tag&&!empty) {
            if(closing){if(stack.empty()||stack.back().first!=name)throw std::runtime_error("翻译段落标签不匹配");stack.pop_back();}
            else stack.emplace_back(name,piece);
        }
        i=end;
        if(!tag&&piece.size()==1&&std::string(".!?; \n").find(piece.front())!=std::string::npos&&current.size()>4500)flush();
    }
    if(!current.empty())parts.push_back(current+close());
    for(const auto& part:parts)if(part.size()>max_request)throw std::runtime_error("翻译段落无法在上限内安全拆分");
    return parts;
}
std::string marker(std::size_t index){return "<!--rss-segment-"+std::to_string(index)+"-->";}
struct Builder {
    TranslationDocument document;
    std::size_t visited=0;
    std::string segment(const std::string& html,bool title=false) {
        const auto probe=make_block(html,document.blocks.size());
        auto parts=probe.request.size()>max_request?split_html(html):std::vector<std::string>{html};
        std::string out;
        for(const auto& part:parts) {
            if(document.blocks.size()>=2048)throw std::runtime_error("可翻译段落超过上限");
            auto block=make_block(part,document.blocks.size());
            if(block.request.size()>max_request)throw std::runtime_error("翻译段落超过请求上限");
            if(title)block.source_units=0;
            out+=marker(document.blocks.size());document.blocks.push_back(std::move(block));
        }
        return out;
    }
    std::string children(xmlNode* start,int depth=0) {
        std::string out,run;
        const auto flush=[&]{if(!run.empty()){out+=translation_units(html_to_text(run))?segment(run):run;run.clear();}};
        for(auto node=start;node;node=node->next) {
            if(depth>64||++visited>10000)throw std::runtime_error("翻译文档结构超过限制");
            if(node->type==XML_TEXT_NODE){if(node->content)run+=escape(reinterpret_cast<const char*>(node->content));continue;}
            if(node->type!=XML_ELEMENT_NODE)continue;
            const std::string tag(reinterpret_cast<const char*>(node->name));
            if(tag=="pre"||tag=="img") {
                flush();
                std::unique_ptr<xmlBuffer,decltype(&xmlBufferFree)> buffer(xmlBufferCreate(),xmlBufferFree);
                if(!buffer)throw std::bad_alloc();xmlNodeDump(buffer.get(),node->doc,node,0,0);
                const std::string html(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),xmlBufferLength(buffer.get()));
                out+=segment(html);continue;
            }
            if(blocks.count(tag)) {
                flush();const auto first=document.blocks.size();
                out+="<"+tag+attributes(node,false)+(tag=="hr"?"/>":">"+children(node->children,depth+1)+"</"+tag+">");
                if(tag=="li"&&first<document.blocks.size())++document.blocks[first].source_units;
            }else {
                std::unique_ptr<xmlBuffer,decltype(&xmlBufferFree)> buffer(xmlBufferCreate(),xmlBufferFree);
                if(!buffer)throw std::bad_alloc();xmlNodeDump(buffer.get(),node->doc,node,0,0);
                run.append(reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),xmlBufferLength(buffer.get()));
            }
        }
        flush();return out;
    }
};
}

std::string translation_revision(const Entry& entry) {
    if(entry.html.size()>max_input||entry.text.size()>max_input||entry.title.size()>16384)throw std::runtime_error("文章超出翻译输入大小限制");
    std::string images;
    for(const auto& image:entry.images){images+=image.url;images+='\0';}
    return "rss-translation-v2:"+fingerprint(entry.title)+":"+fingerprint(entry.html)+":"+fingerprint(entry.text)+":"+fingerprint(entry.url)+":"+fingerprint(images);
}
std::size_t translation_units(std::string_view text) {
    std::size_t count=0;
    for(std::size_t i=0;i<text.size();) {
        const auto length=utf8_length(text,i);const auto c=static_cast<unsigned char>(text[i]);
        if(!((length==1&&c<=32)||(length==2&&c==0xc2&&static_cast<unsigned char>(text[i+1])==0xa0)))++count;
        i+=length;
    }
    return count;
}
TranslationDocument translation_document(const Entry& entry) {
    bounds(entry);Builder builder;builder.document.revision=translation_revision(entry);
    builder.segment(escape(entry.title),true);builder.document.title_blocks=builder.document.blocks.size();
    std::string html;
    if(entry.html.empty()||html_to_text(entry.html)!=entry.text) {
        std::istringstream input(entry.text);std::string line;
        while(std::getline(input,line))html+="<p>"+escape(line)+"</p>";
    }else {
        auto doc=parse(entry.html);std::size_t count=0;html=canonical(xmlDocGetRootElement(doc.get()),entry,false,count);
    }
    if(!html.empty()) {Fragment fragment(html);builder.document.html=builder.children(fragment.root->children);}
    if(builder.document.html.size()>max_output)throw std::runtime_error("翻译文档超过大小限制");
    return std::move(builder.document);
}
std::string translate_block(const TranslationBlock& block,const std::function<std::string(const std::string&)>& translate) {
    if(block.request.empty())return block.html;
    Fragment fragment(block.request);
    const auto text=[&](const std::string& source) {
        if(translation_units(source)==0||std::all_of(source.begin(),source.end(),[](unsigned char c){return c<128&&!std::isalpha(c);}))return source;
        const auto first=source.find_first_not_of(" \t\r\n"),last=source.find_last_not_of(" \t\r\n");
        const auto input=source.substr(first,last-first+1);
        TranslationBlock plain;plain.request=escape(input);
        const auto response=translate(input);
        validate_translation(plain,response);
        return source.substr(0,first)+response+source.substr(last+1);
    };
    const auto replace=[&](auto&& self,xmlNode* node)->void {
        while(node) {
            auto next=node->next;
            if(node->type==XML_TEXT_NODE||node->type==XML_CDATA_SECTION_NODE) {
                const std::string source=node->content?reinterpret_cast<const char*>(node->content):"";
                std::string translated;
                for(std::size_t at=0;at<source.size();) {
                    auto end=source.size();std::size_t token_size=0;
                    for(const auto& item:block.protected_markup) {
                        const auto found=source.find(item.first,at);
                        if(found<end){end=found;token_size=item.first.size();}
                    }
                    translated+=text(source.substr(at,end-at));
                    if(token_size)translated+=source.substr(end,token_size);
                    at=end+token_size;
                }
                auto replacement=xmlNewText(BAD_CAST translated.c_str());if(!replacement)throw std::bad_alloc();
                xmlReplaceNode(node,replacement);xmlFreeNode(node);
            }else self(self,node->children);
            node=next;
        }
    };
    // The provider translates text only; source markup never leaves local control.
    replace(replace,fragment.root->children);
    const auto response=dump(fragment.root->children);
    validate_translation(block,response);
    return response;
}
std::string validate_translation(const TranslationBlock& block,const std::string& response) {
    try {
        if(response.empty()||response.size()>64*1024)throw std::runtime_error("无效响应");
        translation_units(response);
        if(signature(response)!=signature(block.request))throw std::runtime_error("行内结构变化");
        Fragment fragment(response);std::size_t count=0;
        auto safe=canonical(fragment.root->children,Entry{},true,count);
        std::size_t cursor=0;
        while((cursor=safe.find("RSSKEEP",cursor))!=std::string::npos) {
            auto end=cursor+7;const auto digits=end;
            while(end<safe.size()&&std::isdigit(static_cast<unsigned char>(safe[end])))++end;
            if(end==digits){cursor=end;continue;}
            const auto separator=end;while(end<safe.size()&&safe[end]=='X')++end;
            const auto ordinal=end;while(end<safe.size()&&std::isdigit(static_cast<unsigned char>(safe[end])))++end;
            if(separator==ordinal||end==ordinal||end==safe.size()||safe[end]!='Q'){cursor=end;continue;}
            const auto token=safe.substr(cursor,end-cursor+1);
            if(std::none_of(block.protected_markup.begin(),block.protected_markup.end(),[&](const auto& item){return item.first==token;})&&block.request.find(token)==std::string::npos)
                throw std::runtime_error("未知占位符");
            cursor=end+1;
        }
        for(const auto& item:block.protected_markup) {
            const auto at=safe.find(item.first);
            if(at==std::string::npos||safe.find(item.first,at+item.first.size())!=std::string::npos)throw std::runtime_error("占位符变化");
            safe.replace(at,item.first.size(),item.second);
        }
        if(safe.size()>max_output||translation_units(html_to_text(safe))==0)throw std::runtime_error("空译文");
        return safe;
    }catch(const std::exception&){throw std::runtime_error("翻译响应校验失败，请手动重试该段");}
}
Entry translated_entry(const Entry& source,const TranslationDocument& doc,const std::vector<std::string>& translations,ReadingMode mode) {
    bounds(source);
    if(doc.revision!=translation_revision(source)||doc.title_blocks==0||doc.title_blocks>doc.blocks.size())throw std::runtime_error("翻译缓存与原文不匹配");
    Entry out=source;out.reading_mode=mode;out.reading_anchors.clear();
    if(mode==ReadingMode::Original) {
        std::size_t position=0;
        for(std::size_t i=doc.title_blocks;i<doc.blocks.size();++i){out.reading_anchors.push_back({position,position});position+=doc.blocks[i].source_units;}
        return out;
    }
    if(translations.size()!=doc.blocks.size())throw std::runtime_error("翻译缓存段落数量不匹配");
    std::string title;bool translated_title=true;
    for(std::size_t i=0;i<doc.title_blocks;++i) {
        if(doc.blocks[i].request.empty())title+=html_to_text(doc.blocks[i].html);
        else if(translations[i].empty()){translated_title=false;break;}
        else title+=html_to_text(validate_translation(doc.blocks[i],translations[i]));
    }
    if(!translated_title)title=source.title;
    out.title=mode==ReadingMode::Bilingual&&title!=source.title?source.title+"\n"+title:title;
    if(out.title.size()>65536)throw std::runtime_error("译文标题超过大小限制");
    std::size_t source_position=0,display_position=0,cursor=0;
    out.html.clear();
    for(std::size_t i=doc.title_blocks;i<doc.blocks.size();++i) {
        const auto& block=doc.blocks[i];const auto token=marker(i);const auto at=doc.html.find(token,cursor);
        if(at==std::string::npos)throw std::runtime_error("翻译文档段落索引无效");
        out.html+=doc.html.substr(cursor,at-cursor);cursor=at+token.size();
        const auto original_units=translation_units(html_to_text(block.html));
        const auto prefix=block.source_units-original_units;
        std::string translated;
        if(!block.request.empty()&&!translations[i].empty())translated=validate_translation(block,translations[i]);
        std::string replacement=block.html;
        if(!block.request.empty()) {
            if(mode==ReadingMode::Translated)replacement=translated.empty()?block.html+"<br/>（本段未翻译）":translated;
            else replacement=block.html+"<br/><span class=\"rss-translation\">"+(translated.empty()?"（本段未翻译）":without_images(translated))+"</span>";
        }
        out.reading_anchors.push_back({source_position,display_position});
        if(mode==ReadingMode::Bilingual&&!translated.empty())out.reading_anchors.push_back({source_position,display_position+original_units+prefix});
        source_position+=block.source_units;display_position+=translation_units(html_to_text(replacement))+prefix;
        out.html+=replacement;if(out.html.size()>max_output)throw std::runtime_error("译文正文超过大小限制");
    }
    out.html+=doc.html.substr(cursor);out.text=html_to_text(out.html);
    if(out.html.size()>max_output||out.text.size()>max_output)throw std::runtime_error("译文正文超过大小限制");
    return out;
}
}
