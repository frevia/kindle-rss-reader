const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(__dirname + '/../home/card.js', 'utf8');
function harness(title = 'Kindle Unlimited 现已升级为 Kindle+', sharedRoot) {
    let now = 0, timerId = 0, timers = new Map(), values = [], stateCursor = 0, mounted = false, cleanup;
    const callbacks = {}, calls = {connect: [], close: [], send: [], open: [], updates: 0};
    const root = sharedRoot || {
        setTimeout(fn, delay) { timers.set(++timerId, {fn, due: now + delay}); return timerId; },
        clearTimeout(id) { timers.delete(id); }
    };
    if (!sharedRoot) { vm.createContext(root); vm.runInContext(source, root); }
    const emit = (type, event) => { for (const fn of callbacks[type] || []) fn(event); };
    const RN = {View: "View", Text: "Text", Image: "UnsupportedImage", TouchableOpacity: "TouchableOpacity", DeviceEventEmitter: {addListener(type, fn) {
        (callbacks[type] ||= new Set()).add(fn); return {remove() { callbacks[type].delete(fn); }};
    }}, NativeModules: {
        WebSocketModule: {
            connect(...args) { calls.connect.push(args); },
            send(...args) { calls.send.push(args); },
            close(...args) { calls.close.push(args); emit('websocketClosed', {id: args[2]}); }
        },
        NavigationModule: {navigateTo(...args) { calls.open.push(args); }}
    }};
    const React = {
        createElement(type, props, ...children) { return {type, props: {...props, children}}; },
        useState(initial) { const slot = stateCursor++; if (!mounted) values[slot] = initial; return [values[slot], next => { const value = typeof next === "function" ? next(values[slot]) : next; if (value !== values[slot]) { values[slot] = value; calls.updates++; } }]; },
        useEffect(effect) { if (!mounted) { mounted = true; cleanup = effect(); } }
    };
    const props = {cardItem: {campaignId: 'cloud-id-can-change', title, cardData: [{imagePath: '/old', metadata: 1}], buttonActionsurl: '/store'}};
    const original = JSON.stringify(props);
    function render() { stateCursor = 0; return root.useRssCard(React, RN, props); }
    function advance(ms) {
        now += ms;
        for (const [id, timer] of Array.from(timers)) if (timer.due <= now) { timers.delete(id); timer.fn(); }
    }
    function message(data, id = calls.connect.at(-1)?.[3]) {
        emit('websocketMessage', {id, type: 'text', data: typeof data === 'string' ? data : JSON.stringify(data)});
    }
    function stop() { if (cleanup) cleanup(); mounted = false; }
    return {root, calls, callbacks, props, original, render, emit, advance, message, stop, timers};
}
function snapshot(character, changes = {}) {
    return {type: 'rss-home-v1', revision: character.repeat(64), status: 'ready', unread: 2, feeds: 3,
        updatedAt: 123, items: [{id: 1, title: '中文文章与引号“测试”', source: '独立博客'}], ...changes};
}
const h = harness();
assert.equal(h.root.global, undefined, 'Kindle has no global alias');
let view = h.render();
assert.equal(view.cardItem.title, 'RSS订阅');
assert.equal(h.calls.connect[0][0], 'ws://127.0.0.1:12681/rss-home/v1');
assert.equal(h.calls.connect[0][1], null, 'an empty protocols array would abort on this firmware');
assert.equal(JSON.stringify(h.calls.connect[0][2]), '{}');
assert.equal(view.cardItem.cardData[0].imagePath, '/mnt/us/native-rss/book-cover.png');
view.rssHomeOpen();
assert.equal(h.calls.open[0][0], 'file:///mnt/us/documents/RSS阅读器.sh');
const firstId = h.calls.connect[0][3];
h.message(snapshot('a'), firstId + 1); h.message('{bad');
h.message(snapshot('g')); h.message(snapshot('a', {unread: -1}));
h.message(snapshot('a', {enabled: 'false'})); h.message(snapshot('a', {enabled: 0}));
h.message(snapshot('a', {items: [{id: 1, title: 'x'.repeat(101), source: ''}]}));
assert.equal(h.calls.updates, 0);
h.message(snapshot('a', {feedCards: [{id: 1, title: 'bad', unread: -1, latestTitle: ''}]}));
h.message(snapshot('a', {feedCards: [{id: 1, title: '', unread: 0, latestTitle: '', entryId: -1}]}));
h.message(snapshot('a', {feedCards: [{id: 1, title: '', unread: 0, latestTitle: '', entryId: '42;rm'}]}));
h.message(snapshot('a', {feedCards: [{id: 1, title: 'x'.repeat(51), unread: 1, latestTitle: ''}]}));
h.message(snapshot('a', {feedCards: Array.from({length: 4}, (_, id) => ({id, title: '', unread: 0, latestTitle: ''}))}));
h.message(snapshot('a', {feedCards: [{id: 1, title: '', unread: 0, latestTitle: ''}, {id: 1, title: '', unread: 0, latestTitle: ''}]}));
for (const bad of [{excerpt: null}, {excerpt: 1}, {excerpt: 'x'.repeat(321)}, {published: '123'},
    {published: -1}, {published: 1.5}, {published: 8640000000001}]) {
    h.message(snapshot('a', {feedCards: [{id: 1, title: '测试', unread: 0, latestTitle: '标题', ...bad}]}));
}
assert.equal(h.calls.updates, 0, 'malformed feed summaries cannot replace the cached snapshot');
h.message(snapshot('a')); view = h.render();
assert.equal(view.cardItem.titleText, '中文文章与引号“测试”');
assert.equal(view.cardItem.title, 'RSS订阅 · 2 条未读');
h.message(snapshot('a')); assert.equal(h.calls.updates, 1); assert.equal(h.calls.send.length, 2);
assert.equal(JSON.parse(h.calls.send[0][0]).revision, 'a'.repeat(64));
h.message(snapshot('b', {unread: 0})); assert.equal(h.render().cardItem.title, 'RSS订阅 · 已全部读完');
h.emit('websocketFailed', {id: firstId}); assert.equal(h.calls.close.length, 1);
assert.equal(h.render().cardItem.title, 'RSS订阅 · 已全部读完');
h.advance(1000); assert.equal(h.calls.connect.length, 2);
const secondId = h.calls.connect[1][3]; assert.notEqual(firstId, secondId);
h.message(snapshot('c'), firstId); assert.equal(h.calls.updates, 2);
h.message(snapshot('a')); assert.equal(h.render().cardItem.title, 'RSS订阅 · 2 条未读', 'revision equality, not numeric order, supports service restarts');
h.message(snapshot('d', {status: 'unavailable', items: [], unread: 0}));
assert.equal(h.render().cardItem.title, 'RSS订阅 · 2 条未读', 'transient errors preserve a good cached card');
assert.equal(JSON.stringify(h.props), h.original, 'original native props remain untouched');
h.stop(); assert.equal(h.timers.size, 0);
for (const list of Object.values(h.callbacks)) assert.equal(list.size, 0);
h.advance(60000); assert.equal(h.calls.connect.length, 2);
view = h.render(); assert.equal(view.cardItem.title, 'RSS订阅 · 2 条未读');
assert.equal(h.calls.connect.length, 3); h.stop();
for (const title of ['试用 Kindle+', 'Try Kindle Unlimited', 'Kindle+ Free Trial']) {
    const trial = harness(title);
    const hidden = trial.render();
    assert.equal(hidden.rssHomeRender({}), null, 'collapse the later trial placement instead of repeating the shelf');
    assert.equal(trial.calls.connect.length, 0, 'hidden promotion does not create another RSS subscription');
    assert.equal(JSON.stringify(trial.props), trial.original);
    trial.stop();
}
const other = harness('另一条公告'); assert.equal(other.render(), other.props); assert.equal(other.calls.connect.length, 0); other.stop();
const empty = harness(); empty.render(); empty.message(snapshot('e', {status: 'empty', items: [], unread: 0, feeds: 0}));
assert.equal(empty.render().cardItem.titleText, '打开阅读器添加订阅'); empty.stop();
const visibility = harness(); visibility.render();
visibility.message(snapshot('a', {enabled: false}));
assert.equal(visibility.render().rssHomeRender(null), null, 'disabled RSS shelf collapses entirely');
assert.equal(visibility.calls.connect.length, 1);
visibility.message(snapshot('b', {status: 'unavailable'}));
assert.equal(visibility.render().rssHomeRender(null), null, 'temporary database errors cannot re-enable a hidden shelf');
visibility.stop();
assert.equal(visibility.render().rssHomeRender(null), null, 'cached visibility survives native card remount');
visibility.message(snapshot('c', {enabled: true}));
assert.equal(visibility.render().cardItem.title, 'RSS订阅 · 2 条未读', 'same component restores RSS when enabled again');
assert.equal(visibility.calls.connect.length, 2, 'reconnect on remount; visibility changes keep the socket alive');
visibility.message(snapshot('d', {enabled: false}));
assert.equal(visibility.render().rssHomeRender(null), null);
visibility.message(snapshot('e'));
assert.equal(visibility.render().cardItem.title, 'RSS订阅 · 2 条未读', 'older bridge without the field defaults to enabled');
visibility.stop();
const cardA = harness(), cardB = harness('Kindle+', cardA.root);
cardA.render(); cardB.render();
cardA.message(snapshot('f')); cardB.message(snapshot('f'));
assert.equal(cardA.render().cardItem.title, 'RSS订阅 · 2 条未读');
assert.equal(cardB.render().cardItem.title, 'RSS订阅 · 2 条未读', 'each mounted card updates even when another card already cached the revision');
cardA.stop(); cardB.stop();
const timeout = harness(); timeout.render(); timeout.advance(12000); assert.equal(timeout.calls.close.length, 1);
timeout.advance(1000); assert.equal(timeout.calls.connect.length, 2); timeout.stop();
const silent = harness(); silent.render();
const silentId = silent.calls.connect[0][3];
silent.emit('websocketOpen', {id: silentId});
silent.message(snapshot('a'));
const beforeHeartbeat = silent.calls.updates, beforeAck = silent.calls.send.length;
silent.advance(30000);
silent.message({type: 'rss-home-heartbeat'});
assert.equal(silent.calls.updates, beforeHeartbeat);
assert.equal(silent.calls.send.length, beforeAck);
silent.advance(30000); assert.equal(silent.calls.connect.length, 1);
silent.advance(15000); assert.equal(silent.calls.close.length, 1, 'silent native disconnect is caught by liveness timeout');
silent.advance(1000); assert.equal(silent.calls.connect.length, 2);
silent.stop(); assert.equal(silent.timers.size, 0);
const layout = harness(); layout.render();
layout.message(snapshot('a', {items: [
    {id: 1, title: '第一篇文章标题', source: '博客甲'},
    {id: 2, title: '第二篇文章标题很长也保持在单独的一行', source: '博客乙'}
], feedCards: [
    {id: 7, title: '博客甲', unread: 2, latestTitle: '第一篇文章标题', entryId: 42, excerpt: '正文摘录包含中文、English 与标点。', published: 1793005260},
    {id: 8, title: '博客乙', unread: 120, latestTitle: '第二篇文章标题很长也保持在单独的一行', entryId: 77, excerpt: '😀'.repeat(160), published: 0},
    {id: 9, title: '', unread: 0, latestTitle: ''}
]}));
const layoutProps = layout.render();
class NativePrimary { constructor(props) { this.props = props; } render() { return {type: "KindleButton", props: this.props}; } }
class NativeImage {}
const originalCard = {key: "original", props: {
    testID: "Card_native", style: [{marginHorizontal: 12}, {height: 360}],
    children: [{type: "Text", props: {style: {fontFamily: "Kindle native heading", fontSize: 32}}}, {props: {children: [{type: NativeImage, props: {}}, {props: {children: [
        {type: "Text", props: {style: {fontFamily: "Kindle native body", fontSize: 28, flexShrink: 1}}}, {type: NativePrimary, props: {testID: "Card_native_button"}}
    ]}}]}}]
}};
const card = layoutProps.rssHomeRender(originalCard);
function flatten(node) {
    if (!node || typeof node !== 'object') return [node];
    if (Array.isArray(node)) return node.flatMap(flatten);
    return [node, ...node.props.children.flatMap(flatten)];
}
const nodes = flatten(card), texts = nodes.filter(node => node?.type === 'Text');
assert.equal(card.props.testID, 'Card_native', 'retain native card identity');
assert.equal(card.props.key, 'original');
assert.equal(card.props.style[0][1].height, 360, 'retain the original firmware dimensions as the scale reference');
const arrows = nodes.filter(node => node?.type === NativeImage);
assert.equal(arrows.length, 1, 'only the section heading uses an image; article cards retain native text');
assert.equal(nodes.some(node => node?.type === 'UnsupportedImage'), false, 'SVG uses the original firmware image wrapper');
assert.equal(arrows[0].props.source.uri, 'file://KPPHome/Fill.svg', 'reuse the native homepage chevron');
assert.equal(arrows[0].props.style.width / arrows[0].props.style.height, 11 / 19);
assert.equal(arrows[0].props.accessible, false, 'decorative arrow does not add a second touch target');
assert.equal(texts.some(node => node.props.children[0] === '〉'), false, 'no font glyph with a separate baseline');
assert.equal(card.props.children[1].props.style.flexDirection, 'row');
for (const title of ['第一篇文章标题', '第二篇文章标题很长也保持在单独的一行']) {
    const node = texts.find(node => node.props.children[0] === title);
    assert.equal(node.props.numberOfLines, 3);
    assert.equal(node.props.style.flexShrink, 0, 'long titles keep their multi-line text area');
    assert.equal(node.props.style.height, undefined, 'native title measurement leaves spare lines for the excerpt');
    assert.equal(node.props.style.fontFamily, "Kindle native body");
    assert.equal(node.props.ellipsizeMode, 'tail');
}
assert.ok(texts.find(node => node.props.children[0] === '博客甲'));
assert.ok(texts.find(node => node.props.children[0] === '2 条未读'));
const action = nodes.find(node => node?.type === 'KindleButton');
assert.equal(action.props.testID, 'Card_native_button', 'retain the firmware button event binding');
assert.equal(action.props.invertMode, "none");
assert.equal(action.props.style.height, 50, 'the native section heading is the reader touch target');
assert.equal(texts.find(node => node.props.children[0] === 'RSS订阅').props.style.fontFamily, 'Kindle native heading');
const books = nodes.filter(node => node?.type === 'KindleButton' && node.props.testID.includes('_feed_'));
assert.equal(books.length, 3, 'one stable book card per feed');
assert.deepEqual(books.map(node => node.props.key), ['rss-feed-7', 'rss-feed-8', 'rss-feed-9']);
for (const book of books) {
    const cover = book.props.children[0];
    assert.equal(cover.props.style.width / cover.props.style.height, 2 / 3, 'native portrait cover ratio');
    assert.equal(book.props.children.length, 1, 'all reading information stays inside the cover');
    const coverTexts = flatten(cover).filter(node => node?.type === 'Text');
    assert.ok(coverTexts.length >= 5 && coverTexts.length <= 10, 'source, title, excerpt rows, date and unread count stay inside the cover');
    assert.ok(coverTexts[1].props.children[0]);
    book.props.onPress();
}
assert.ok(texts.find(node => node.props.children[0] === '99+ 未读'), 'large unread counts fit the cover badge');
assert.ok(texts.find(node => node.props.children[0] === '未命名订阅'));
assert.ok(texts.find(node => node.props.children[0] === '暂无文章'));
const visibleText = list => list.filter(node => node?.type === 'Text').map(node => node.props.children[0]).join('');
assert.ok(visibleText(nodes).includes('正文摘录包含中文、English'));
const excerptArea = list => list.find(node => node?.props?.testID === 'Card_native_button_feed_7_excerpt');
assert.equal(layout.root.rssHomeSnapshot.feedCards[1].excerpt.length, 320, 'UTF-16 validation permits 160 supplementary codepoints');
for (const node of texts) for (const letter of node.props.children[0]) { const cp = letter.codePointAt(0); assert.ok(cp < 0xd800 || cp > 0xdfff, 'wrapping never splits surrogate pairs'); }
const published = new Date(1793005260 * 1000);
assert.ok(texts.find(node => node.props.children[0] === `${published.getMonth() + 1}月${published.getDate()}日`));
assert.ok(visibleText(nodes).includes('打开阅读器同步文章'));
const shortTitle = texts.find(node => node.props.children[0] === '第一篇文章标题');
const excerptBefore = excerptArea(nodes);
shortTitle.props.onLayout({nativeEvent: {layout: {height: shortTitle.props.style.lineHeight}}});
const measuredNodes = flatten(layout.render().rssHomeRender(originalCard));
const excerptAfter = excerptArea(measuredNodes);
assert.ok(excerptAfter.props.style.height > excerptBefore.props.style.height, 'short titles give their unused space to the excerpt');
const articleArea = books[0].props.children[0].props.children[1].props.style;
assert.ok(shortTitle.props.style.lineHeight + excerptAfter.props.style.marginTop + excerptAfter.props.style.height <= articleArea.height - articleArea.paddingTop);
const measuredUpdates = layout.calls.updates;
shortTitle.props.onLayout({nativeEvent: {layout: {height: shortTitle.props.style.lineHeight + 0.1}}});
for (const height of [0, -1, NaN, Infinity, 1000]) shortTitle.props.onLayout({nativeEvent: {layout: {height}}});
shortTitle.props.onLayout({});
assert.equal(layout.calls.updates, measuredUpdates, 'unchanged or invalid layout events cannot trigger a redraw loop');
const unsupported = {props: {style: [{}, {height: 'auto'}]}};
assert.equal(layoutProps.rssHomeRender(unsupported), unsupported, 'unknown native dimensions keep the working fallback');
action.props.onPress(999);
assert.equal(layout.calls.open.length, 4, 'all three book cards and the section header open the reader');
assert.deepEqual(layout.calls.open.map(call => call[0]), [
    'file:///mnt/us/native-rss/home/open/entry-42.sh',
    'file:///mnt/us/native-rss/home/open/entry-77.sh',
    'file:///mnt/us/documents/RSS阅读器.sh',
    'file:///mnt/us/documents/RSS阅读器.sh'
], 'each card keeps its own article ID; empty feeds and header open the list');
layout.message(snapshot('b', {feedCards: undefined}));
const legacy = flatten(layout.render().rssHomeRender(originalCard));
assert.equal(legacy.filter(node => node?.type === 'KindleButton' && node.props.testID.includes('_feed_')).length, 1);
assert.ok(legacy.find(node => node?.type === 'Text' && node.props.children[0] === 'RSS 阅读器'), 'older bridge retains a working aggregate reader card');
layout.message(snapshot('d', {feedCards: [{id: 7, title: '博客甲', unread: 1, latestTitle: '替换为一篇新的长标题', entryId: 43, excerpt: '新的摘录'}]}));
const replaced = flatten(layout.render().rssHomeRender(originalCard));
assert.equal(excerptArea(replaced).props.style.height, excerptBefore.props.style.height, 'a new title does not reuse a previous title measurement');
layout.message(snapshot('e', {feedCards: [{id: 7, title: '旧后台', unread: 1, latestTitle: '已有文章', entryId: 42}]}));
const oldBridge = flatten(layout.render().rssHomeRender(originalCard));
assert.ok(visibleText(oldBridge).includes('打开文章，继续阅读'));
assert.ok(oldBridge.find(node => node?.type === 'Text' && node.props.children[0] === ''), 'missing publication date stays blank');
layout.message(snapshot('f', {feedCards: [{id: 7, title: '博客甲', unread: 1, latestTitle: '【每周一书】掌控习惯', entryId: 42}]}));
const columnTitle = flatten(layout.render().rssHomeRender(originalCard));
assert.ok(columnTitle.find(node => node?.type === 'Text' && node.props.children[0] === '【每周一书】\n掌控习惯'), 'column prefix wraps as a complete phrase');
assert.ok(columnTitle.find(node => node?.type === 'KindleButton' && node.props.accessibilityLabel.includes('【每周一书】掌控习惯')), 'spoken title and entry action preserve the original article');
layout.message(snapshot('a', {feedCards: [{id: 7, title: '博客甲', unread: 1, latestTitle: '排版测试', entryId: 42,
    excerpt: '一二三四五六七八，九十。English words stay intact.'}]}));
const wrapped = flatten(layout.render().rssHomeRender(originalCard));
const wrappedRows = flatten(excerptArea(wrapped)).filter(node => node?.type === 'Text').map(node => node.props.children[0]);
assert.ok(wrappedRows.length >= 2);
assert.ok(wrappedRows.every(row => !/^[，。！？；：、）》】]/u.test(row)), 'closing punctuation never starts a wrapped row');
assert.ok(wrappedRows.join('').startsWith('一二三四五六七八，九十。'), 'wrapping does not lose or duplicate characters');
layout.message(snapshot('b', {feedCards: [{id: 7, title: '博客甲', unread: 1, latestTitle: '排版测试', entryId: 42,
    excerpt: '书籍：《掌控习惯》 《掌控习惯》是一本书。'}]}));
const bookExcerpt = visibleText(flatten(excerptArea(flatten(layout.render().rssHomeRender(originalCard)))));
assert.ok(bookExcerpt.startsWith('《掌控习惯》是一本书。'), 'only the repeated opening book label is removed');
layout.message(snapshot('c', {status: 'empty', items: [], unread: 0, feedCards: [{id: 10, title: '空订阅', unread: 0, latestTitle: ''}]}));
const emptyShelf = flatten(layout.render().rssHomeRender(originalCard));
assert.ok(emptyShelf.find(node => node?.type === 'Text' && node.props.children[0] === '空订阅'));
layout.stop();
console.log('home card: validation, immutable props, cache, reconnect, cleanup, native book shelf, and fixed reader actions passed');
