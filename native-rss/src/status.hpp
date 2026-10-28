#pragma once
#include <ctime>
#include <string>

namespace rss {
struct ReaderStatus {
    int battery=-1;
    bool charging=false,wifi=false;
    std::string clock;
    bool operator==(const ReaderStatus&) const;
    bool operator!=(const ReaderStatus&) const;
};
ReaderStatus read_reader_status(std::time_t now,const std::string& root="/");
}
