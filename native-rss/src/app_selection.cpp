#include "app.hpp"
#include "translation.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace rss {
namespace {
std::size_t next_character(const std::string& text,std::size_t at){
    if(at>=text.size())return text.size();
    ++at;while(at<text.size()&&(static_cast<unsigned char>(text[at])&0xc0)==0x80)++at;
    return at;
}
bool word_character(unsigned char c){return c<128&&(std::isalnum(c)||c=='\''||c=='-'||c=='_');}
std::pair<std::size_t,std::size_t> word(const std::string& text,std::size_t at){
    auto begin=at,end=next_character(text,at);
    if(at<text.size()&&word_character(static_cast<unsigned char>(text[at]))){
        while(begin&&word_character(static_cast<unsigned char>(text[begin-1])))--begin;
        while(end<text.size()&&word_character(static_cast<unsigned char>(text[end])))++end;
    }
    return {begin,end};
}
std::optional<std::size_t> classic_offset(const std::string& anchor){
    if(anchor.rfind("classic:",0)!=0)return std::nullopt;
    const auto value=anchor.substr(8);
    if(value.empty()||!std::all_of(value.begin(),value.end(),[](unsigned char c){return c>='0'&&c<='9';}))return std::nullopt;
    try{const auto offset=std::stoull(value);if(offset<=std::numeric_limits<std::size_t>::max())return static_cast<std::size_t>(offset);}catch(const std::exception&){}
    return std::nullopt;
}
bool same_job(const SelectionJob& a,const SelectionJob& b){
    return a.entry_id==b.entry_id&&a.web==b.web&&a.mode==b.mode&&a.revision==b.revision&&a.range.start==b.range.start&&a.range.end==b.range.end&&a.range.text==b.range.text;
}
}
void App::clear_selection(){
    selection_.reset();selection_drag_.reset();selection_result_.clear();selection_error_.clear();selection_loading_=false;selection_result_page_=0;
    if(reader_panel_==ReaderPanel::Selection){reader_panel_=ReaderPanel::None;reader_tools_=false;}
    dirty=true;
}
void App::load_selection_marks(){
    selection_marks_.clear();selection_revision_.clear();
    if(!current_)return;
    try{
        selection_revision_=translation_revision(*current_);
        selection_marks_=store_.selections(current_->id,current_->web_content,current_->reading_mode,selection_revision_);
    }catch(const std::exception&){selection_revision_.clear();}
}
SelectionJob App::selection_job()const{
    if(!current_||!selection_||selection_revision_.empty())throw std::runtime_error("请重新选择正文中的文字");
    return {current_->id,current_->web_content,current_->reading_mode,selection_revision_,selection_->range};
}
std::vector<Rect> App::selection_rects(const SelectionRange& range){
    if(!current_)return {};
    if(cre_)return cre_->selection_rects(page_,range);
    const auto start=classic_offset(range.start),end=classic_offset(range.end);
    const auto& text=current_->text;
    if(!start||!end||*start>=*end||*end>text.size()||text.substr(*start,*end-*start)!=range.text)return {};
    std::vector<Rect> rectangles;
    const int heading=page_==0?article_heading_height():0;
    for(const auto& element:pages_[page_].elements){
        if(element.image>=0)continue;
        const auto a=std::max(*start,element.line.start),b=std::min(*end,element.line.end);
        if(a>=b)continue;
        const auto offset=std::min(a-element.line.start,element.line.text.size());
        const auto length=std::min(b-a,element.line.text.size()-offset);
        const int left=element.rect.x+c_.measure(element.line.text.substr(0,offset),element.size);
        const int width=c_.measure(element.line.text.substr(offset,length),element.size);
        if(width>0)rectangles.push_back({left,heading+element.rect.y,width,element.rect.h});
    }
    return rectangles;
}
std::optional<ReaderSelection> App::classic_selection(TouchPoint start,TouchPoint end,bool adjusting){
    const auto character=[this,adjusting](TouchPoint point)->std::optional<std::size_t>{
        const int heading=page_==0?article_heading_height():0;
        const PageElement* chosen=nullptr;
        std::pair<int,int> nearest{std::numeric_limits<int>::max(),std::numeric_limits<int>::max()};
        for(const auto& element:pages_[page_].elements){
            if(element.image>=0||element.line.text.empty())continue;
            const int top=96+heading+element.rect.y,left=margin_+element.rect.x;
            const int width=c_.measure(element.line.text,element.size);
            if(!adjusting){
                if(point.y>=top&&point.y<top+element.rect.h&&point.x>=left&&point.x<left+width){chosen=&element;break;}
            }else{
                if(top+element.rect.h>c_.height-72||translation_units(element.line.text)==0)continue;
                const int dy=std::max({top-point.y,point.y-(top+element.rect.h-1),0});
                const int dx=std::max({left-point.x,point.x-(left+width-1),0});
                if(std::pair<int,int>{dy,dx}<nearest){nearest={dy,dx};chosen=&element;}
            }
        }
        if(!chosen)return std::nullopt;
        const int left=margin_+chosen->rect.x;
        std::optional<std::size_t> closest;int distance=std::numeric_limits<int>::max();
        for(std::size_t at=0;at<chosen->line.text.size();){
            const auto next=next_character(chosen->line.text,at);
            const int right=c_.measure(chosen->line.text.substr(0,next),chosen->size);
            if(!adjusting){if(right>point.x-left)return chosen->line.start+at;}
            else if(static_cast<unsigned char>(chosen->line.text[at])>32){
                const int x=left+c_.measure(chosen->line.text.substr(0,at),chosen->size);
                const int dx=std::max({x-point.x,point.x-(left+right-1),0});
                if(dx<distance){distance=dx;closest=chosen->line.start+at;}
                if(dx==0||x>point.x)break;
            }
            at=next;
        }
        return closest;
    };
    const auto a=character(start),b=character(end);if(!a||!b)return std::nullopt;
    const auto first=word(current_->text,std::min(*a,*b)),last=word(current_->text,std::max(*a,*b));
    if(last.second<=first.first||last.second-first.first>6000)return std::nullopt;
    SelectionRange range{"classic:"+std::to_string(first.first),"classic:"+std::to_string(last.second),current_->text.substr(first.first,last.second-first.first)};
    if(translation_units(range.text)==0)return std::nullopt;
    auto rectangles=selection_rects(range);if(rectangles.empty())return std::nullopt;
    return ReaderSelection{std::move(range),std::move(rectangles)};
}
void App::select_text(TouchPoint start,TouchPoint end,bool adjusting){
    if(!current_||selection_revision_.empty())return;
    auto chosen=cre_?cre_->select(page_,start.x-margin_,start.y-96,end.x-margin_,end.y-96,adjusting):classic_selection(start,end,adjusting);
    if(!chosen||chosen->rects.empty()||chosen->range.text.empty()||chosen->range.text.size()>6000)return;
    selection_=std::move(chosen);selection_result_.clear();selection_error_.clear();selection_loading_=false;selection_result_page_=0;dirty=true;
}
void App::draw_selection_marks(int clip_bottom){
    const auto clipped=[&](Rect rect){
        rect.x+=margin_;rect.y+=96;
        const int right=std::min(c_.width-margin_,rect.x+rect.w),bottom=std::min(clip_bottom,rect.y+rect.h);
        rect.x=std::max(margin_,rect.x);rect.y=std::max(96,rect.y);rect.w=right-rect.x;rect.h=bottom-rect.y;
        return rect;
    };
    for(const auto& mark:selection_marks_)if(mark.highlighted)for(auto rect:selection_rects(mark.range)){
        rect=clipped(rect);if(rect.w>0&&rect.h>1)c_.fill({rect.x,rect.y+rect.h-2,rect.w,2},30);
    }
    if(!selection_)return;
    selection_->rects=selection_rects(selection_->range);
    if(selection_->rects.empty()){clear_selection();return;}
    for(auto rect:selection_->rects){
        rect=clipped(rect);if(rect.w<=0||rect.h<=0)continue;
        for(int y=rect.y;y<rect.y+rect.h;++y)for(int x=rect.x;x<rect.x+rect.w;++x){auto& pixel=c_.pixels[static_cast<std::size_t>(y)*c_.width+x];pixel=std::min<std::uint8_t>(pixel,205);}
    }
    if(reader_panel_==ReaderPanel::None&&!selection_->rects.empty()){
        const auto& a=selection_->rects.front();const auto& b=selection_->rects.back();
        for(const auto point:{TouchPoint{margin_+a.x,96+a.y+a.h},TouchPoint{margin_+b.x+b.w,96+b.y+b.h}})
            if(point.y+16<clip_bottom)c_.fill({std::clamp(point.x-4,margin_,c_.width-margin_-8),point.y,8,16},30);
    }
}
void App::selection_tools(){
    constexpr int width=240,height=80,gap=24;
    int left_edge=selection_->rects.front().x,right_edge=left_edge;
    for(const auto& rect:selection_->rects){
        left_edge=std::min(left_edge,rect.x);right_edge=std::max(right_edge,rect.x+rect.w);
    }
    const int left=std::clamp(margin_+(left_edge+right_edge-width)/2,32,c_.width-width-32);
    const auto& first=selection_->rects.front();const auto& last=selection_->rects.back();
    int top=96+first.y-height-gap;
    if(top<96)top=96+last.y+last.h+gap;
    top=std::clamp(top,96,c_.height-72-height);
    c_.fill({left,top,width,height},30);c_.fill({left+2,top+2,width-4,height-4},255);
    button({left,top,width,height},"翻译",[this]{translate_selected_text();},false,28);
}
void App::translate_selected_text(){
    const auto job=selection_job();const auto saved=store_.selection(job);
    selection_result_page_=0;selection_error_.clear();selection_result_=saved?saved->translation:std::string{};
    reader_tools_=true;reader_panel_=ReaderPanel::Selection;
    if(!selection_result_.empty())return;
    if(queued_translation_=="translate-selection"&&pending_selection_&&same_job(*pending_selection_,job)){
        selection_loading_=true;
        return;
    }
    if(translation_blocked())return;
    if(std::none_of(job.range.text.begin(),job.range.text.end(),[](unsigned char c){return c<128&&std::isalpha(c);})){
        selection_error_="请选择英文单词或句子，再翻译为中文。";return;
    }
    pending_selection_=job;selection_loading_=true;translation_request_.clear();start_translation("translate-selection");
}
std::optional<SelectionJob> App::take_selection_request(){auto job=std::move(pending_selection_);pending_selection_.reset();return job;}
void App::selection_finished(const SelectionJob& job,const std::string& translated,bool cancelled,const std::string& error){
    cancelled=cancelled&&translated.empty();
    automatic_sync_=false;translating_=true;sync_total_=1;sync_completed_=error.empty()&&!cancelled?1:0;sync_failed_=error.empty()?0:1;
    sync_state_=cancelled?SyncState::Cancelled:!error.empty()?SyncState::Failed:SyncState::Complete;
    sync_message_=cancelled?"选区翻译已取消":error.empty()?"选区译文已保存":error;
    if(selection_&&current_&&same_job(selection_job(),job)){
        selection_loading_=false;selection_result_=translated;selection_error_=cancelled?"翻译已取消，可手动重试。":error;selection_result_page_=0;
    }
    if(selection_||screen_==Screen::Sync)dirty=true;
}
void App::selection_panel(int y,int height){
    if(!selection_)return;
    const auto source=c_.wrap(selection_->range.text,24,c_.width-64);
    for(std::size_t i=0;i<std::min<std::size_t>(2,source.size());++i)
        c_.text(32,y+104+static_cast<int>(i)*34,i==1&&source.size()>2?c_.ellipsize(source[i].text+"…",24,c_.width-64):source[i].text,24,85);
    c_.rule(32,y+166,c_.width-64,210);
    const bool queued=selection_loading_&&queued_translation_=="translate-selection";
    const bool blocked=translation_blocked();
    const auto message=queued?"正在结束本轮自动同步，随后开始翻译选中文字。":
        selection_loading_?"正在翻译选中文字…":!selection_error_.empty()?selection_error_:
        !selection_result_.empty()?selection_result_:blocked?"当前有任务正在进行，完成后可翻译选中文字。":
        "点下方按钮，将选中的英文翻译为中文。";
    const auto lines=c_.wrap(message,26,c_.width-64);constexpr std::size_t visible=6;
    const auto count=std::max<std::size_t>(1,(lines.size()+visible-1)/visible);selection_result_page_=std::min(selection_result_page_,count-1);
    const auto start=selection_result_page_*visible;
    for(std::size_t i=0;i<visible&&start+i<lines.size();++i)c_.text(32,y+212+static_cast<int>(i)*36,lines[start+i].text,26);
    const int controls=y+height-92;c_.rule(32,controls-8,c_.width-64,210);
    if(selection_loading_)button({32,controls,c_.width-64,80},queued?"取消等待":"取消翻译",[this,queued]{if(queued)cancel_queued_translation();else request_="cancel-sync";},false,28,Icon::Close);
    else if(!selection_result_.empty()){
        button({32,controls,200,80},"上一页",[this]{--selection_result_page_;},false,28,Icon::Back,selection_result_page_>0);
        const auto label=std::to_string(selection_result_page_+1)+" / "+std::to_string(count);c_.text((c_.width-c_.measure(label,24))/2,controls+48,label,24,85);
        button({c_.width-232,controls,200,80},"下一页",[this]{++selection_result_page_;},false,28,Icon::Next,selection_result_page_+1<count);
    }else {
        const auto label=blocked?"等待当前任务完成":
            selection_error_.empty()?"翻译为中文":"重新翻译";
        button({32,controls,c_.width-64,80},label,[this]{translate_selected_text();},false,28,Icon::None,!blocked);
    }
}
}
