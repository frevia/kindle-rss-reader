// Firmware adapter entry; this is the actual Hermes global object, not a `global` alias.
function useRssCard(React, RN, props) {
    var root = this;
    var item = props.cardItem;
    var title = typeof item.title === "string" ? item.title : "";
    var target = title.indexOf("Kindle+") >= 0 || title.indexOf("Kindle Unlimited") >= 0;
    // The later trial promotion must not repeat the RSS shelf above Discover Books.
    var trial = target && (title.indexOf("试用") >= 0 || title.indexOf("Try ") >= 0 || title.indexOf("Free Trial") >= 0);
    var state = React.useState(target && !trial ? root.rssHomeSnapshot || null : null);
    var snapshot = state[0];
    var update = state[1];
    React.useEffect(function () {
        if (!target || trial) return;
        var active = true;
        var current = snapshot;
        var socket = null;
        var retry = null;
        var watchdog = null;
        var delay = 1000;
        var module = RN.NativeModules.WebSocketModule;
        function clearWatchdog() {
            if (watchdog !== null) root.clearTimeout(watchdog);
            watchdog = null;
        }
        function armWatchdog(milliseconds) {
            clearWatchdog();
            watchdog = root.setTimeout(reconnect, milliseconds);
        }
        function closeSocket() {
            var previous = socket;
            socket = null;
            clearWatchdog();
            if (previous !== null) {
                try { module.close(1000, "rss-home", previous); } catch (error) {}
            }
        }
        function reconnect() {
            if (!active) return;
            closeSocket();
            if (retry !== null) return;
            retry = root.setTimeout(connect, delay);
            delay = Math.min(delay * 2, 30000);
        }
        function connect() {
            retry = null;
            if (!active) return;
            var next = (root.rssHomeSocketId || 700000) + 1;
            if (next > 1073741824) next = 700001;
            root.rssHomeSocketId = next;
            socket = next;
            armWatchdog(12000);
            try {
                // The device rejects every non-null protocols value, including [].
                module.connect("ws://127.0.0.1:12681/rss-home/v1", null, {}, socket);
            } catch (error) { reconnect(); }
        }
        function validNumber(value) {
            return typeof value === "number" && value >= 0 && value <= 9007199254740991 && value % 1 === 0;
        }
        function receive(event) {
            if (!active || event.id !== socket || event.type !== "text" || typeof event.data !== "string" || event.data.length > 8192) return;
            try {
                var data = JSON.parse(event.data);
                // This firmware can silently lose a native socket without a close event.
                if (data && data.type === "rss-home-heartbeat") { armWatchdog(45000); return; }
                if (!data || data.type !== "rss-home-v1" || typeof data.revision !== "string" || data.revision.length !== 64 ||
                    (data.enabled !== undefined && typeof data.enabled !== "boolean") ||
                    !validNumber(data.unread) || !validNumber(data.feeds) || !validNumber(data.updatedAt) ||
                    (data.status !== "ready" && data.status !== "empty" && data.status !== "unavailable") ||
                    !Array.isArray(data.items) || data.items.length > 2) return;
                for (var j = 0; j < 64; j++) if ("0123456789abcdef".indexOf(data.revision[j]) < 0) return;
                var items = [];
                for (var i = 0; i < data.items.length; i++) {
                    var entry = data.items[i];
                    if (!entry || !validNumber(entry.id) || typeof entry.title !== "string" || entry.title.length > 100 ||
                        typeof entry.source !== "string" || entry.source.length > 50) return;
                    items.push({id: entry.id, title: entry.title, source: entry.source});
                }
                var feedCards = [];
                if (data.feedCards !== undefined) {
                    if (!Array.isArray(data.feedCards) || data.feedCards.length > 3) return;
                    for (var k = 0; k < data.feedCards.length; k++) {
                        var feed = data.feedCards[k];
                        if (!feed || !validNumber(feed.id) || !validNumber(feed.unread) ||
                            typeof feed.title !== "string" || feed.title.length > 50 ||
                            typeof feed.latestTitle !== "string" || feed.latestTitle.length > 100 ||
                            (feed.entryId !== undefined && !validNumber(feed.entryId))) return;
                        for (var prior = 0; prior < feedCards.length; prior++) if (feedCards[prior].id === feed.id) return;
                        feedCards.push({id: feed.id, title: feed.title, unread: feed.unread, latestTitle: feed.latestTitle, entryId: feed.entryId || 0});
                    }
                }
                var clean = {revision: data.revision, status: data.status, enabled: data.enabled !== false, unread: data.unread,
                    feeds: data.feeds, updatedAt: data.updatedAt, items: items, feedCards: feedCards};
                armWatchdog(45000);
                if ((!current || clean.revision !== current.revision) &&
                    !(clean.status === "unavailable" && current && current.status !== "unavailable")) {
                    current = clean;
                    root.rssHomeSnapshot = clean;
                    update(clean);
                }
                // A restarted bridge needs a fresh acknowledgement even for cached content.
                module.send(JSON.stringify({type: "rss-home-ack", revision: clean.revision}), socket);
            } catch (error) {}
        }
        var message = RN.DeviceEventEmitter.addListener("websocketMessage", receive);
        var opened = RN.DeviceEventEmitter.addListener("websocketOpen", function (event) {
            if (active && event.id === socket) { delay = 1000; armWatchdog(45000); }
        });
        var failed = RN.DeviceEventEmitter.addListener("websocketFailed", function (event) {
            if (active && event.id === socket) reconnect();
        });
        var closed = RN.DeviceEventEmitter.addListener("websocketClosed", function (event) {
            if (active && event.id === socket) reconnect();
        });
        connect();
        return function () {
            active = false;
            if (retry !== null) root.clearTimeout(retry);
            closeSocket();
            message.remove(); opened.remove(); failed.remove(); closed.remove();
        };
    }, [target, trial]);
    if (!target) return props;
    if (trial || (snapshot && snapshot.enabled === false)) {
        var hidden = Object.assign({}, props);
        hidden.rssHomeRender = function () { return null; };
        return hidden;
    }
    var heading = "RSS订阅";
    var text = "正在读取本地订阅…";
    if (snapshot) {
        if (snapshot.status === "unavailable") text = "打开阅读器添加订阅并同步";
        else if (snapshot.status === "empty") text = snapshot.feeds ? "还没有本地文章，请先同步" : "打开阅读器添加订阅";
        else {
            heading += snapshot.unread ? " · " + snapshot.unread + " 条未读" : " · 已全部读完";
            text = snapshot.items.length ? snapshot.items[0].title : "还没有本地文章";
            if (snapshot.items.length > 1) text += "\n" + snapshot.items[1].title;
        }
    }
    var images = Array.isArray(item.cardData) ? item.cardData.slice() : [];
    images[0] = Object.assign({}, images[0]);
    images[0].imagePath = "/mnt/us/native-rss/book-cover.png";
    var nextItem = Object.assign({}, item);
    nextItem.title = heading; nextItem.titleText = text; nextItem.buttonText = "打开 RSS 阅读器";
    nextItem.buttonActionsurl = ""; nextItem.customActionsKey = null; nextItem.cardData = images;
    var next = Object.assign({}, props);
    next.cardItem = nextItem;
    next.rssHomeOpen = function (entryId) {
        var uri = "file:///mnt/us/documents/RSS阅读器.sh";
        if (typeof entryId === "number" && entryId > 0 && entryId <= 9007199254740991 && entryId % 1 === 0)
            uri = "file:///mnt/us/native-rss/home/open/entry-" + entryId + ".sh";
        RN.NativeModules.NavigationModule.navigateTo(uri, {});
    };
    next.rssHomeRender = function (original) {
        var create = (React.default || React).createElement;
        var originalStyle = original.props.style;
        var height = Array.isArray(originalStyle) && originalStyle[1] && originalStyle[1].height;
        if (typeof height !== "number" || !(height > 0 && height <= 4096)) return original;
        var nativeButton = original.props.children[1].props.children[1].props.children[1];
        if (!root.rssHomeButtonBase) {
            // The primary wrapper forces black; its base retains Kindle's native touch handling.
            var probeProps = Object.assign({}, nativeButton.props);
            probeProps.children = create(RN.Text, null, "");
            var probe = new nativeButton.type(probeProps);
            root.rssHomeButtonBase = probe.render().type;
        }
        var unit = height / 360;
        var nativeHeading = original.props.children[0];
        var nativeBody = original.props.children[1].props.children[1].props.children[0];
        function label(value, base, size, weight, lines) {
            // Keep the firmware's font family and scaling instead of a separate widget theme.
            var style = Object.assign({}, base);
            style.margin = 0; style.marginLeft = 0; style.marginRight = 0;
            style.marginTop = 0; style.marginBottom = 0; style.padding = 0;
            style.height = lines > 2 ? size * 1.35 * lines : undefined;
            style.flexShrink = 0; style.fontSize = size;
            style.lineHeight = size * 1.35; style.fontWeight = weight;
            style.color = "#000000";
            var textProps = {};
            textProps.style = style; textProps.numberOfLines = lines;
            textProps.ellipsizeMode = "tail";
            return create(RN.Text, textProps, value);
        }
        var headingStyle = nativeHeading.props.style;
        var bodyStyle = nativeBody.props.style;
        var titleSize = headingStyle.fontSize;
        var bodySize = bodyStyle.fontSize;
        if (!(titleSize > 0) || !(bodySize > 0)) return original;
        var frame = {};
        frame.paddingLeft = 32 * unit; frame.paddingRight = 0;
        frame.paddingTop = 14 * unit; frame.paddingBottom = 22 * unit;
        frame.height = 440 * unit;
        var frameProps = Object.assign({}, original.props);
        frameProps.key = original.key; frameProps.style = [originalStyle, frame];
        var header = {}; header.height = 50 * unit; header.width = "100%";
        header.flexDirection = "row"; header.alignItems = "center";
        header.justifyContent = "space-between"; header.backgroundColor = "#ffffff";
        header.borderWidth = 0;
        var headerProps = Object.assign({}, nativeButton.props);
        headerProps.style = header; headerProps.invertMode = "none";
        headerProps.onPress = function () { next.rssHomeOpen(); }; headerProps.accessibilityLabel = "打开 RSS 阅读器";
        var name = {}; name.flexDirection = "row"; name.alignItems = "center";
        var nameProps = {}; nameProps.style = name;
        var arrow = {}; arrow.marginLeft = 20 * unit;
        var arrowProps = {}; arrowProps.style = arrow;
        var unread = snapshot && (snapshot.status === "ready" || snapshot.status === "empty") ?
            (snapshot.unread ? snapshot.unread + " 条未读" : "已读完") : "";
        var headingView = create(root.rssHomeButtonBase, headerProps,
            create(RN.View, nameProps,
                label("RSS订阅", headingStyle, titleSize, "bold", 1),
                create(RN.View, arrowProps, label("〉", headingStyle, titleSize * 1.2, "bold", 1))),
            label(unread, bodyStyle, bodySize * 0.72, "normal", 1));
        var body = {}; body.flex = 1; body.flexDirection = "row";
        body.paddingTop = 12 * unit;
        var bodyProps = {}; bodyProps.style = body;
        var feeds = snapshot && snapshot.feedCards && snapshot.feedCards.length ? snapshot.feedCards : [];
        if (!feeds.length) {
            var fallback = {};
            fallback.id = "reader"; fallback.title = "RSS 阅读器";
            fallback.unread = snapshot ? snapshot.unread : 0;
            fallback.latestTitle = snapshot && snapshot.items.length ? snapshot.items[0].title : text;
            feeds = [fallback];
        }
        var books = [];
        for (var index = 0; index < feeds.length; index++) {
            var entry = feeds[index];
            var book = {}; book.width = 225 * unit; book.height = 337.5 * unit;
            book.marginRight = index < feeds.length - 1 ? 27 * unit : 0;
            book.flexDirection = "column"; book.alignItems = "flex-start";
            book.justifyContent = "flex-start"; book.borderWidth = 0;
            book.backgroundColor = "#ffffff";
            var bookProps = Object.assign({}, nativeButton.props);
            bookProps.key = "rss-feed-" + entry.id;
            bookProps.testID = nativeButton.props.testID + "_feed_" + entry.id;
            bookProps.style = book; bookProps.invertMode = "none";
            bookProps.onPress = (function (entryId) {
                return function () { next.rssHomeOpen(entryId); };
            })(entry.entryId || 0);
            bookProps.accessibilityLabel = (entry.title || "未命名订阅") + "，" + (entry.latestTitle || "暂无文章") + "，" + entry.unread + " 条未读，" + (entry.entryId ? "打开文章详情" : "打开 RSS 阅读器");
            var cover = {}; cover.width = 225 * unit; cover.height = 337.5 * unit;
            cover.borderWidth = unit; cover.borderColor = "#777777";
            cover.backgroundColor = "#ffffff";
            cover.paddingLeft = 18 * unit; cover.paddingRight = 18 * unit;
            cover.paddingTop = 14 * unit; cover.paddingBottom = 14 * unit;
            var coverProps = {}; coverProps.style = cover;
            var source = {}; source.height = 62 * unit;
            source.justifyContent = "center"; source.borderBottomWidth = unit;
            source.borderBottomColor = "#aaaaaa"; source.paddingBottom = 8 * unit;
            var sourceProps = {}; sourceProps.style = source;
            var article = {}; article.height = 219.5 * unit; article.paddingTop = 18 * unit;
            var articleProps = {}; articleProps.style = article;
            var footer = {}; footer.height = 28 * unit;
            footer.alignItems = "flex-end"; footer.justifyContent = "flex-end";
            var footerProps = {}; footerProps.style = footer;
            var unreadText = entry.unread ? (entry.unread > 99 ? "99+" : String(entry.unread)) + " 未读" : "已读完";
            if (!snapshot || snapshot.status === "unavailable") unreadText = "";
            var coverView = create(RN.View, coverProps,
                create(RN.View, sourceProps,
                    label(entry.title || "未命名订阅", headingStyle, 18 * unit, "bold", 2)),
                create(RN.View, articleProps,
                    label(entry.latestTitle || "暂无文章", bodyStyle, 27 * unit, "normal", 5)),
                create(RN.View, footerProps,
                    label(unreadText, bodyStyle, 17 * unit, "normal", 1)));
            books.push(create(root.rssHomeButtonBase, bookProps, coverView));
        }
        return create(RN.View, frameProps, headingView, create(RN.View, bodyProps, books));
    };
    return next;
}
