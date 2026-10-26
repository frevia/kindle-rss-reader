#include "net.hpp"
#include <cassert>
#include <iostream>

int main(){
    rss::Store store(":memory:");rss::Feed healthy,failed;
    healthy.url="https://healthy.invalid/rss";failed.url="https://failed.invalid/rss";
    auto a=store.add_feed(healthy),b=store.add_feed(failed);store.sync_result(b,"previous failure","","",0);
    std::atomic<bool> cancel{false};int total=-1;
    // Cancel at the initial progress event: no test request reaches the network.
    rss::sync_feeds(store,cancel,[&](const rss::SyncProgress& p){total=p.total;cancel=true;},true);
    assert(total==1);assert(store.feeds()[0].id==a&&store.feeds()[0].error.empty());assert(store.feeds()[1].error=="previous failure");
    store.sync_result(b,"","","",0);cancel=false;
    rss::sync_feeds(store,cancel,[&](const rss::SyncProgress& p){total=p.total;},true);assert(total==0);
    std::cout<<"retry checks passed\n";
}
