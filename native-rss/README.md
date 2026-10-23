# Kindle Native RSS

面向 KPW3 的独立原生 RSS 阅读器。C++17、FreeType 灰度渲染、SQLite 离线存储、libxml2 订阅解析和 libcurl 联网；桌面使用 SDL 展示相同的渲染结果。不是 KOReader 插件，也不是网页套壳。

**当前是开发验证版，尚不是真机验收完成的 Kindle 成品。** ARM 静态构建已安装到 KPW3；开发包提供中文字体和证书。单次启动退出、休眠、应用及启动器强杀恢复已有真机证据，拼音搜索选词经用户确认。离线连续翻页、USB 退出、OPML、阅读位置、字号与收藏已通过用户实机验证；长期使用仍待验收。完整范围见 [实施计划](docs/PLAN.md)，设备证据见 [DEVICE.md](docs/DEVICE.md)。

## 本机构建

需要 CMake、pkg-config、C++17 编译器及 libxml2、sqlite3、libcurl（支持 CURLOPT_PROTOCOLS_STR）、freetype2、libwebp、sdl2 开发库。

```sh
rtk proxy cmake -S native-rss -B native-rss/build -DCMAKE_BUILD_TYPE=Debug
rtk proxy cmake --build native-rss/build -j 6
rtk proxy ctest --test-dir native-rss/build --output-on-failure
```

从本仓库根目录运行。测试使用临时数据库与本机 HTTP 服务，不接触 Kindle。Linux 的中文视觉检查需要 `RSS_FONT` 指向合适字体；测试 fallback 字体不代表中文显示验收。

## ARM 构建状态

Apple Silicon macOS 上的依赖构建脚本为 `tools/build_arm.py`，依赖版本与校验来源在 `tools/arm-sources.json`。需要 CMake、Make、Perl、curl 和 Python 3.12+，以及 `vendor/zig-aarch64-macos-0.15.2/zig`。Zig 官方归档为 `https://ziglang.org/download/0.15.2/zig-aarch64-macos-0.15.2.tar.xz`，SHA-256 为 `3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b`。编译器目前需预先放置在该位置。

```sh
rtk proxy python3 native-rss/tools/build_arm.py
```

产物为 `build-arm/kindle-rss`，目标 ARMv7 Cortex-A9、EABI5 soft-float、静态链接。脚本检查 ELF 类型、浮点 ABI、程序段边界及动态链接段；这些检查不证明旧内核兼容性。当前产物约 8.5 MB，已在 KPW3 5.16.2.1.1 运行并验证图文渲染。设备端 `device` 运行循环与 `device/launch.sh` 会话启动器已接通；不要绕过启动器的入口检查。KUAL 入口位于 `device/kual/`，仍非完成验收的安装包。

ARM 网络访问使用开发包中的 `/mnt/us/native-rss/certs/ca-bundle.pem`，也可通过 `RSS_CA_BUNDLE` 指定。不能依赖构建主机上的 OpenSSL 证书路径。

## 生成开发包

```sh
rtk proxy python3 native-rss/tools/package.py
```

需要先完成 ARM 构建。工具验证源码及二进制哈希，拒绝过期产物；缺少运行资源时按 `tools/runtime-sources.json` 下载并验证。输出 `output/kindle-rss-kpw3-dev.zip` 和 `output/kindle-rss-source-dev.zip`，不自动安装。包含 Noto Sans SC（OFL 1.1）、Mozilla 根证书（MPL 2.0）、KUAL 入口、第三方许可、安装说明和逐文件 SHA-256 清单。程序使用未修改的可变字体，设置字重为 400。

FreeType 按 FreeType License 使用。Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.

## 使用

```sh
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite import /absolute/path/feeds.opml
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite sync
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite desktop
```

桌面窗口支持点击、左右方向键翻页、Esc 返回、系统输入法及 Cmd/Ctrl+V 粘贴。同步按钮再次点击可取消更新。新内容不会自动重排当前列表；点击更新提示刷新。

GUI 的导入按钮读取数据库同目录下的 `feeds.opml`；导出写入 `subscriptions.opml`。CLI 的 `import` 和 `export` 可指定其他路径。OPML 仅含订阅与分组，不包含阅读状态或正文。

在「我的阅读」中点击订阅旁的「管理」，可查看订阅地址、同步错误，整源标为已读或取消订阅。取消订阅需要确认，只保留收藏文章；后台已开始的下载不会恢复已取消订阅的文章。「缓存管理」可确认清理 30 天前的已读文章，保留收藏、未读及正在阅读的文章。当前清理会删除文章及状态；订阅源仍提供的文章可能在下次同步后重新出现。

「订阅 / 分组」可切换导航方式。父分组汇总其全部子分组，例如「科技」包含「科技/编程」，不包含「科技新闻」；分组内搜索只检索该分组。管理页可修改显示名称和分组，分组留空即移到「未分组」。名称修改不会改变订阅地址、文章状态或同步凭据；后续同步保留自定义名称。原生软键盘支持拼音选字和词组，桌面仍可使用系统输入法。

`--font /absolute/path/font.ttf` 或 `RSS_FONT` 可以指定字体。macOS 默认使用本机系统字体，仅本机读取，不随项目分发。

## 渲染与示例

示例只在显式调用 `demo` 时写入，不伪装成真实订阅。使用独立数据库，避免混入个人订阅。

```sh
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite demo
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite render native-rss/output/list.pgm
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite --article 1 render native-rss/output/reader.pgm
```

生成 1072×1448 的 PGM 灰度图，采用与应用相同的布局代码。渲染命令不改变文章已读状态。

## 当前行为与限制

- RSS 2/RDF/Atom、嵌套 OPML 导入导出；导入按 URL 去重，保留已有名称和分组。
- 同步使用 ETag/Last-Modified；TLS 校验开启；单源失败不删除旧数据，不阻止其他源更新。
- 单份订阅限制 8 MiB，连接超时 10 秒，总请求超时 35 秒。
- 正文按原文顺序图文分页：图片等比缩放，放不下时整张移到下一页；短 figcaption 与图片保持同页，较长图注继续分页。点击正文图片可打开对应大图，返回保持当前页。保留旧文字阅读位置，额外记录连续图片页的位置。未做通用网页全文提取，不能保证摘要源的全文离线。
- 同步缓存正文 `<img>` 的 HTTP(S) 地址，支持相对地址和 `data-src`，每篇最多 20 张、每次同步处理最多 100 张。支持 JPEG/PNG/静态 WebP，单文件最多 8 MiB、边长不超过 2048；JPEG/PNG 解码分配预算 32 MiB；缩小为适合屏幕的灰度缓存，透明像素合成白底。动态 WebP、GIF、SVG、srcset 和 enclosure 图片尚不支持。
- 图片失败单独记录并显示原因，不删除文字；下次同步重试。已缓存的相同 URL 不重复下载；源站在同一 URL 替换图片时暂不自动失效。图片与文章一起清理，收藏图片随收藏保留。缓存放在 SQLite 中，删除释放的页面会被后续写入复用，文件未必立即缩小。
- 已读、收藏、字符位置独立于正文更新。列表只加载元数据，打开文章时再读正文。
- GUI 原生软键盘支持中英文切换、候选翻页、英文大小写、空格与网址符号。搜索、订阅名称和分组默认使用拼音；连续输入完整词组拼音（例如 keji），ü 用 v。使用固定词频的离线候选，不提供模糊拼音、整句预测和用户词频学习。尚未选词时点击确定会提示继续选词，切换英文会保留拼音原文。KPW3 的 keji→科技 搜索选词已经用户确认，长输入和候选翻页仍需真机覆盖。
- 首次没有订阅会展示导入/添加入口。标题搜索、视图切换、列表与正文分页、字号/行距/页边距已接通。
- 无服务器账号、无遥测、无对 Kindle 的自动安装；数据库和导出的 OPML 可能包含私人订阅地址，应自行保管。

## 验证状态

核心解析、存储、排版、页面交互与本地 HTTP 集成测试已接入 CTest。设备的单次退出、休眠和强杀恢复已验证；重复生命周期、USB、更多真实 RSS 兼容范围及电量表现尚未完整验收。完成标准以实施计划为准，不以测试通过数量代替产品完成。

静态 WebP 使用 libwebp 1.6.0，随包附带 COPYING 和 PATENTS。JPEG/PNG 解码使用固定提交的 stb_image，来源、校验值和许可保存在 `third_party/stb/`。测试图片由本项目生成，包含正常图片、过大尺寸和受限解压输入。

拼音词库来自 [Rime pinyin-simp](https://github.com/rime/rime-pinyin-simp)，采用 Apache 2.0 许可；固定提交、原始文件、许可和校验值位于 `third_party/pinyin/`。`python3 tools/generate_pinyin.py` 校验源文件后生成按拼音和词频排序的只读表，包含 65,125 个候选。程序使用二分查找，不在输入时加载或扫描完整词库；词库随原生二进制离线提供。
