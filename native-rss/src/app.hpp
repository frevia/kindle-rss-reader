#pragma once
#include "render.hpp"
#include "store.hpp"
#include <functional>

namespace rss {
class App {
public:
    App(Store& store, Canvas& canvas);
    void draw();
    void tap(int x, int y);
    void next();
    void previous();
    void back();
    void text_input(const std::string& value);
    void backspace();
    void submit();
    void reload();
    void open(std::int64_t id, bool track = true);
    void notify(const std::string& text);
    void show_error(const std::string& text);
    void show_view(View view, std::int64_t feed = 0);
    void show_group(const std::string& group);
    std::string take_request();
    bool dirty = true, quit = false;
    bool reading() const { return current_.has_value(); }
    std::size_t reader_page() const { return page_; }
    std::int64_t current_id() const { return current_ ? current_->id : 0; }
private:
    struct Hit { Rect rect; std::function<void()> action; };
    Store& store_; Canvas& c_;
    std::vector<Feed> feeds_;
    std::vector<Entry> list_;
    std::vector<std::size_t> list_starts_{0};
    bool list_complete_ = false;
    std::vector<Page> pages_;
    std::vector<Hit> hits_;
    std::optional<Entry> current_;
    View view_ = View::Unread;
    std::int64_t feed_ = 0;
    std::optional<std::string> group_;
    std::size_t list_page_ = 0, page_ = 0, nav_page_ = 0;
    int font_size_ = 32, line_gap_ = 20, margin_ = 64;
    bool navigation_ = false, menu_ = false, info_ = false, shift_ = false;
    bool storage_ = false;
    bool groups_ = false;
    bool images_ = false;
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
    void button(Rect rect,const std::string& label,std::function<void()> action,bool filled=false,int size=26,Icon icon=Icon::None);
    void list_screen();
    void reader_screen();
    void nav_screen();
    void input_screen();
    void reader_menu();
    void info_screen();
    void image_screen();
    bool draw_image(const ArticleImage& image, Rect area);
    int article_heading_height();
    void management_screen();
    void confirmation_screen();
    std::string view_name() const;
    std::string source(std::int64_t id) const;
};
}
