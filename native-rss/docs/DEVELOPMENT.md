# 开发参考

这份文档用于修改代码、编译程序和排查实现问题。首次接触本项目，请先读[安装教程](INSTALL.md)和[使用指南](USAGE.md)。自行编译需要下载名称带 `-source.zip` 的源码包；下面的命令从解压后能看到 `native-rss` 的那一层运行。固定工具链版本、系统路径和技术限制供开发时核对。

面向 KPW3 的独立原生 RSS 阅读器。C++17、FreeType 灰度渲染、SQLite 离线存储、libxml2 订阅解析和 libcurl 联网；桌面使用 SDL 展示相同的渲染结果。不是 KOReader 插件，也不是网页套壳。

**当前是开发验证版，尚不是真机验收完成的 Kindle 成品。** ARM 静态构建已安装到 KPW3；开发包提供中文字体和证书。单次启动退出、休眠、应用及启动器强杀恢复已有真机证据，拼音搜索选词经用户确认。离线连续翻页、USB 退出、OPML、阅读位置、字号与收藏已通过用户实机验证；长期使用仍待验收。完整范围见[实施计划](PLAN.md)，设备证据见 [DEVICE.md](DEVICE.md)。

## 安装到 Kindle

使用 Release 附件安装，无需本机构建。请先阅读 [安装与升级教程](INSTALL.md)，确认运行环境，再合并复制安装包；升级时保留 `native-rss/data/`。

## 本机构建

需要 CMake、pkg-config、C++17 编译器及 libxml2、sqlite3、libcurl（支持 CURLOPT_PROTOCOLS_STR）、freetype2、libwebp、libavif 1.1.0+（启用 libyuv）、sdl2 开发库。

先确认这些库能通过 `pkg-config` 查到；非系统前缀安装时，将对应的 `lib/pkgconfig` 加入 `PKG_CONFIG_PATH`。例如 libavif 安装在自选前缀后，使用 `PKG_CONFIG_PATH=/absolute/prefix/lib/pkgconfig`。不要将 ARM 的依赖目录用于宿主构建。缺少 `libavif` 时 CMake 会停止；已有旧构建目录成功不代表新环境能直接构建。

```sh
rtk proxy cmake -S native-rss -B native-rss/build -DCMAKE_BUILD_TYPE=Debug
rtk proxy cmake --build native-rss/build -j 6
rtk proxy ctest --test-dir native-rss/build --output-on-failure
```

从本仓库根目录运行。测试使用临时数据库与本机 HTTP 服务，不接触 Kindle。Linux 的中文视觉检查需要 `RSS_FONT` 指向合适字体；测试 fallback 字体不代表中文显示验收。

上面的 `rtk proxy` 是本仓库使用的命令代理；没有安装 RTK 时可直接运行其后的命令。仅构建命令行与测试、不需要 SDL 窗口时，可在配置命令追加 `-DRSS_DESKTOP=OFF`。

## ARM 构建状态

Apple Silicon macOS 上的依赖构建脚本为 `tools/build_arm.py`，依赖版本与校验来源在 `tools/arm-sources.json`。需要 CMake 3.22+、Make、Perl、curl 和 Python 3.12+，以及 `vendor/zig-aarch64-macos-0.15.2/zig`。Zig 官方归档为 `https://ziglang.org/download/0.15.2/zig-aarch64-macos-0.15.2.tar.xz`，SHA-256 为 `3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b`。编译器目前需预先放置在该位置。

```sh
rtk proxy python3 native-rss/tools/build_arm.py
```

产物为 `build-arm/kindle-rss`，目标 ARMv7 Cortex-A9、EABI5 soft-float、静态链接。脚本检查 ELF 类型、浮点 ABI、程序段边界及动态链接段；这些检查不证明旧内核兼容性。已在 KPW3 5.16.2.1.1 运行并验证图文渲染。设备端 `device` 运行循环与 `device/launch.sh` 会话启动器已接通；不要绕过启动器的入口检查。KUAL 入口位于 `device/kual/`，仍非完成验收的安装包。

ARM 网络访问使用开发包中的 `/mnt/us/native-rss/certs/ca-bundle.pem`，也可通过 `RSS_CA_BUNDLE` 指定。不能依赖构建主机上的 OpenSSL 证书路径。

## 生成开发包

```sh
rtk proxy python3 native-rss/tools/package.py
```

需要先完成 ARM 构建。工具验证源码及二进制哈希，拒绝过期产物；缺少运行资源时按 `tools/runtime-sources.json` 下载并验证。输出 `output/kindle-rss-kpw3-dev.zip` 和 `output/kindle-rss-source-dev.zip`，不自动安装。包含 Noto Sans SC（OFL 1.1）、Mozilla 根证书（MPL 2.0）、KUAL 入口、第三方许可、安装说明和逐文件 SHA-256 清单。程序使用未修改的可变字体，设置字重为 400。

项目自身的 MIT 许可与第三方组件汇总同时放入安装包；源码包还包含仓库首页、贡献指南、安全反馈和同一组第三方许可。源码按文件类型白名单收集，诊断 TXT、日志、数据库与 OPML 不属于打包输入。发布前仍需核对实际 ZIP 内容，不能只依赖 `.gitignore`。

## GitHub Release

[发布工作流](../../.github/workflows/release.yml) 在推送 `v数字.数字.数字` 标签时运行，也可在标签引用上手动重跑。发布前更新 CMake 与 KUAL 版本号，以及根目录 `CHANGELOG.md`，再提交并推送标签。安装与使用文档始终指向最新版，无需随应用版本改写。工作流使用 Apple Silicon macOS runner，校验固定 Zig 下载哈希，运行宿主 CTest，重新编译 ARM，再调用 `tools/release.py` 整理附件。

产物为文件名以 `-KPW3.zip`、`-source.zip` 结尾的两个压缩包，以及 `SHA256SUMS.txt`；完整文件名由标签自动生成。工作流核对安装清单、二进制、许可和文件哈希后，上传草稿并发布；任何前置步骤失败都不会发布 Release。此过程不改变仓库可见性，也不部署 Kindle。GitHub 编译产物仍需另行实机验证。

已发布附件若仅修订文档，需保留原程序、非文档源码和 `BUILD.json`，在 `native-rss/DOCUMENTATION.json` 中记录应用提交与文档修订提交，并重算安装清单及附件校验值。发布说明同时标注文档更新，不能把文档修订称为重新编译。GitHub 自动生成的源码归档仍对应原版本标签，后补的文档以自定义源码包及其文档修订记录为准。

FreeType 按 FreeType License 使用。Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.

## 在电脑上运行

Kindle 上的按钮操作见[使用指南](USAGE.md)。下面的命令用于电脑上的开发与验证。

```sh
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite import /absolute/path/feeds.opml
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite sync
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite desktop
```

桌面窗口支持点击、左右方向键翻页、Esc 返回、系统输入法及 Cmd/Ctrl+V 粘贴。点击右上角同步图标进入同步页；同步进行中可在该页取消，失败后可只重试失败订阅。离开同步页后任务继续执行。新内容不会自动重排当前列表；点击更新提示刷新。

GUI 的导入按钮读取数据库同目录下的 `feeds.opml`；导出写入 `subscriptions.opml`。CLI 的 `import` 和 `export` 可指定其他路径。OPML 仅含订阅与分组，不包含阅读状态或正文。

点击首页左上角「RSS Reader」展开「订阅 / 退出」。订阅页按分组展示来源，点击来源名称阅读该源文章，点击右侧「编辑」修改名称、RSS 地址或分组；名称可留空，同步成功后自动读取博客名称。修改仅在「保存更改」时写入，放弃或返回会丢弃草稿。同步中的订阅地址需要先取消同步再修改。取消订阅需要确认，只保留收藏文章；后台已开始的下载不会恢复已取消订阅的文章。

首页「全部分组」可切换阅读范围，按名称或完整路径查找分组；父分组包含子分组，例如「科技」包含「科技/编程」，不包含「科技新闻」。列表显示来源、所属分组和日期，收藏只显示标题左侧的实心星标。未读、全部、收藏和文章搜索保留当前阅读范围；「退出搜索」或提交空关键词退出搜索。

订阅页底部「管理分组」可新增、重命名、移动或删除多级分组。点击「子分组」在当前组内创建；新增和编辑时选择「上级分组」，选择「顶层（无上级）」可移回根目录。更换上级会移动整棵子树，保存前仅修改草稿；上级选择支持按完整路径查找，并排除自身及后代。空分组会保留，重名目标不覆盖；删除分组及其子分组时把组内订阅移到「未分组」，保留文章、收藏和阅读状态。底部「OPML」专门导入/导出订阅及空分组。「缓存管理」从同步页进入，可确认清理 30 天前的已读文章，保留收藏、未读及正在阅读的文章。清理会删除这些文章及状态；订阅源仍提供的文章可能在下次同步后重新出现。原生软键盘支持拼音选字和词组，桌面仍可使用系统输入法。

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
- 同步缓存正文 `<img>` 的 HTTP(S) 地址，支持相对地址和 `data-src`，每篇最多 20 张、每次同步处理最多 100 张。支持 JPEG/PNG/静态 WebP/AVIF；JPEG/PNG 最多 8MP，STB 解码分配预算 64 MiB（PNG扫描行与RGBA可同时驻留）；AVIF 单文件最多 8 MiB、单图 8MP、最大边长 16384、单线程解码，使用 libavif、decoder-only libaom 和 libyuv 在 YUV 色彩空间缩放到最多 944×1130 后转换为 RGB。WebP 有损图最多 64MP、无损图最多 8MP，并在解码时缩放。图像解码均离线运行并缩小为适合屏幕的灰度缓存，透明像素合成白底。动态 WebP、GIF、SVG、srcset 和 enclosure 图片尚不支持。
- 图片失败单独记录并显示原因，不删除文字；下次同步重试。已缓存的相同 URL 不重复下载；源站在同一 URL 替换图片时暂不自动失效。图片与文章一起清理，收藏图片随收藏保留。缓存放在 SQLite 中，删除释放的页面会被后续写入复用，文件未必立即缩小。
- 已读、收藏、字符位置独立于正文更新。列表只加载元数据，打开文章时再读正文。
- GUI 原生软键盘支持中英文切换、候选翻页、英文大小写、空格与网址符号。搜索、订阅名称和分组默认使用拼音；连续输入完整词组拼音（例如 keji），ü 用 v。使用固定词频的离线候选，不提供模糊拼音、整句预测和用户词频学习。尚未选词时点击确定会提示继续选词，切换英文会保留拼音原文。KPW3 的 keji→科技 搜索选词已经用户确认，长输入和候选翻页仍需真机覆盖。
- 首次没有订阅会展示导入/添加入口。标题搜索、视图切换、列表与正文分页、字号/行距/页边距已接通。
- 无服务器账号、无遥测、无对 Kindle 的自动安装；数据库和导出的 OPML 可能包含私人订阅地址，应自行保管。

## 性能排查

当前列表首次显示时计算准确总页数，并缓存标题测量结果供后续翻页和视图切换复用；导航图标计算和屏幕差异检测也已优化。[性能报告](PERFORMANCE.md)保留历史对照和复测命令，其中早期按需统计总页数的行为已被后续修改替代。耗时不能据宿主测试推断 Kindle 实屏体验。

## 验证状态

核心解析、存储、排版、页面交互与本地 HTTP 集成测试已接入 CTest。设备的单次退出、休眠和强杀恢复已验证；重复生命周期、USB、更多真实 RSS 兼容范围及电量表现尚未完整验收。完成标准以实施计划为准，不以测试通过数量代替产品完成。

静态 WebP 使用 libwebp 1.6.0；AVIF 使用 libavif 1.4.2、libaom 3.13.2 与固定提交的 libyuv。随包附带对应许可证和专利声明。JPEG/PNG 解码使用固定提交的 stb_image，来源、校验值和许可保存在 `third_party/stb/`。测试图片由本项目生成，包含颜色、透明度、大尺寸 JPEG/PNG/WebP、10 位 AVIF、损坏文件和受限解压输入。libyuv 的官方 Gitiles 归档带有每次下载时间，`sha256_type: tar-content-v1` 校验路径、类型、模式、大小、链接目标和文件内容，忽略归档时间及所有者；其余依赖校验完整归档。

拼音词库来自 [Rime pinyin-simp](https://github.com/rime/rime-pinyin-simp)，采用 Apache 2.0 许可；固定提交、原始文件、许可和校验值位于 `third_party/pinyin/`。`python3 tools/generate_pinyin.py` 校验源文件后生成按拼音和词频排序的只读表，候选数量以生成脚本输出为准。程序使用二分查找，不在输入时加载或扫描完整词库；词库随原生二进制离线提供。

## 维护设备启动环境

这一节供定位启动失败时核对，不需要让安装用户逐项执行或记住命令。先按真实调用链区分入口问题与运行环境问题：KUAL 菜单和 `reader-scriptlet.sh` 最终都调用 `launch.sh`，后者只在选择书库入口时需要 SH_Integration。`launch.sh` 不调用或检查 Hotfix、Scriptlets、微信读书或商店；它单独检查并调用 FBInk。Universal Hotfix 可以提供 Scriptlets 和 FBInk，但不是唯一来源，不应无条件要求安装或重装。用户侧按[安装教程](INSTALL.md)复制并打开应用，只有入口缺失或启动失败时才进入[启动问题排查](TROUBLESHOOTING.md)，按对应现象检查环境或导出检查文件。

启动器的实际条件以 `device/launch.sh` 为准：需要 `/mnt/us/libkh/bin/fbink` 可执行，以及 `timeout`、`flock`、`stat`、`pidof`、`awk`、`dd`、`mktemp`、`mkfifo` 和系统 LIPC 工具；设备密码锁开启时会拒绝启动。当前只验证过 KPW3，屏幕和触摸布局校验不应绕过。

`device/diagnose.sh` 导出部分工具路径、设备参数和启动日志，但不会自动补装组件，也不会逐项覆盖启动器的全部检查。协助用户时先根据报告确认原因，再提供适合其设备的补装步骤。FBInk 的上游来源见[第三方组件说明](../../THIRD_PARTY_NOTICES.md)，程序本身不包含在当前安装包中。

### 本应用与微信读书安装包的边界

- RSS Reader 通过自己的 C++ 程序直接处理文章和屏幕，不调用微信读书运行时、Mesquito 的 `apps` 入口、微信读书的 `weread` 或根目录 `fonts`。字体与证书随 RSS 安装包提供。
- Scriptlets 是可选的共享书库启动能力；两者的 `.sh` 入口都会使用它，但这不等于应用本体共享插件。KUAL 入口不经过 SH_Integration。
- `# DontUseFBInk` 只禁止 SH_Integration 把脚本输出额外绘制到屏幕，不能据此认为 RSS 不需要 FBInk：`launch.sh` 仍在恢复系统画面时执行它。
