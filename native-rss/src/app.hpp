#pragma once
#include "render.hpp"
#include "store.hpp"
#include "status.hpp"
#include "cre_reader.hpp"
#include "device.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace rss {
class App {
public:
    enum class ReaderPanel { None, Type, Images, Info, Content };
    enum class Screen { List, Subscriptions, GroupPicker, Groups, GroupEdit, Opml, Sync, Cache, Settings };
    enum class SyncState { Idle, Running, Complete, Cancelled, Failed };
    App(Store& store, Canvas& canvas, std::int64_t initial_article = 0, const std::string& status_font = {});
    void draw();
    void set_reader_status(const ReaderStatus& status);
    void tap(int x, int y);
    void touch(const TouchGesture& gesture);
    void next();
    void previous();
    void back();
    void text_input(const std::string& value);
    void backspace();
    void submit();
    void reload();
    void open(std::int64_t id, bool track = true);
    void notify(const std::string& text);
    void sync_started();
    void sync_progress(int completed, int total, int failed, const std::string& message);
    void sync_finished(bool cancelled, const std::string& error = "");
    Screen screen() const { return screen_; }
    SyncState sync_state() const { return sync_state_; }
    bool menu_open() const { return brand_menu_; }
    const std::string& query() const { return search_; }
    const std::optional<std::string>& selected_group() const { return group_; }
    void show_error(const std::string& text);
    void show_view(View view, std::int64_t feed = 0);
    void show_group(const std::string& group);
    std::string take_request();
    bool dirty = true, quit = false;
    bool reading() const { return current_.has_value(); }
    std::size_t reader_page() const { return page_; }
    std::int64_t current_id() const { return current_ ? current_->id : 0; }
    bool using_cre() const { return static_cast<bool>(cre_); }
    bool using_fulltext() const { return current_ && current_->web_content; }
    bool reader_tools_open() const { return reader_tools_; }
    ReaderPanel reader_panel() const { return reader_panel_; }
    std::size_t reader_image_page() const { return image_page_; }
private:
    struct Hit { Rect rect; std::function<void()> action; };
    Store& store_; Canvas& c_;
    std::unique_ptr<Canvas> status_canvas_;
    std::vector<Feed> feeds_;
    std::vector<Entry> list_;
    std::vector<std::size_t> list_starts_{0};
    bool list_complete_ = false;
    int list_layout_width_=0,list_layout_top_=0,list_layout_bottom_=0;
    struct RowMetrics { std::string title, metadata; int width, title_lines, meta_lines; };
    std::map<std::int64_t,RowMetrics> row_metrics_;
    std::vector<Page> pages_;
    std::vector<Hit> hits_;
    std::optional<Entry> current_;
    std::unique_ptr<CreReader> cre_;
    bool cre_requested_ = false;
    View view_ = View::Unread;
    std::int64_t feed_ = 0;
    std::optional<std::string> group_;
    std::size_t list_page_ = 0, page_ = 0, nav_page_ = 0;
    int last_tap_x_ = 0;
    int font_size_ = 32, line_gap_ = 20, margin_ = 32;
    ReaderStatus reader_status_;
    bool reader_tools_ = false, image_zoom_ = false, shift_ = false, symbols_ = false;
    ReaderPanel reader_panel_ = ReaderPanel::None;
    std::size_t info_scroll_ = 0;
    std::optional<std::int64_t> typography_anchor_;
    Screen screen_ = Screen::List, edit_origin_ = Screen::Subscriptions;
    SyncState sync_state_ = SyncState::Idle;
    int sync_completed_ = 0, sync_total_ = 0, sync_failed_ = 0;
    std::string sync_message_;
    bool brand_menu_ = false, source_list_ = false;
    struct ListContext { View view; std::int64_t feed; std::optional<std::string> group; std::string search; std::size_t page; };
    std::optional<ListContext> home_;
    std::vector<std::string> group_names_;
    std::set<std::string> folded_;
    std::size_t picker_page_ = 0, groups_page_ = 0, sync_page_ = 0, nav_page_count_ = 1;
    enum class PickerPurpose { Read, Search, Move, Parent };
    PickerPurpose picker_purpose_ = PickerPurpose::Read;
    struct InputContext { std::string mode, value, composing; bool chinese, symbols, shift; std::size_t candidate; };
    std::optional<InputContext> suspended_input_;
    std::string picker_query_, editing_group_, group_name_draft_, group_parent_draft_;
    bool new_group_ = false;
    bool auto_fulltext_ = false;
    std::size_t image_page_=0;
    std::optional<Feed> managing_;
    std::string confirmation_;
    std::string status_, search_, input_, input_mode_, request_, error_;
    bool chinese_ = false;
    std::string composing_;
    std::size_t candidate_page_ = 0;
    void start_input(const std::string& mode,const std::string& value);
    void keyboard_key(const std::string& value);
    void input_space();
    int row_height(const Entry& entry);
    void ensure_list_page(std::size_t page);
    int line_height() const { return font_size_ + line_gap_; }
    void paginate();
    void save_position();
    std::string reader_key(const std::string& prefix) const;
    void set_reader_content(bool web);
    void set_reader_engine(bool use_cre);
    enum class Align { Center, Left, Right };
    void button(Rect rect,const std::string& label,std::function<void()> action,bool filled=false,int size=28,Icon icon=Icon::None,bool enabled=true,Align align=Align::Center);
    void back_button(int top=6,int height=64);
    void page_header(const std::string& title);
    void device_status(bool clock,bool clock_left=false);
    void list_screen();
    void reader_screen();
    void nav_screen();
    void group_picker_screen();
    void group_edit_screen();
    void edit_group(const std::string& path, bool create);
    void opml_screen();
    void sync_screen();
    void app_menu();
    void settings_screen();
    void open_subscriptions();
    void restore_home();
    void open_group_picker(PickerPurpose purpose);
    void restore_input();
    void edit_feed(const Feed& feed);
    void save_feed();
    void clear_search();
    std::string source_group(std::int64_t id) const;
    std::string article_metadata(const Entry& entry) const;
    int list_top() const { return search_.empty() ? 164 : 252; }
    int list_bottom() const { return c_.height - (status_.empty() ? 104 : 208); }
    void input_screen();
    void reader_tools();
    void reader_panel_screen();
    bool draw_image(const ArticleImage& image, Rect area, bool enlarge=false, int clip_bottom=-1);
    void close_reader_tools();
    int reader_panel_height() const;
    int article_heading_height();
    void management_screen();
    void confirmation_screen();
    std::string scope_name() const;
    std::string source(std::int64_t id) const;
};
}
