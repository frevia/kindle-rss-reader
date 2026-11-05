# 开发参考

这份文档用于修改代码、编译程序和排查实现问题。首次接触本项目，请先读[安装教程](INSTALL.md)和[使用指南](USAGE.md)。自行编译需要下载名称带 `-source.zip` 的源码包；下面的命令从解压后能看到 `native-rss` 的那一层运行。固定工具链版本、系统路径和技术限制供开发时核对。

面向 Kindle 的独立原生 RSS 阅读器，当前源码新增按设备能力检查的兼容版，范围见 [COMPATIBILITY.md](COMPATIBILITY.md)。C++17、CREngine 正文排版与 FreeType 灰度渲染、SQLite 离线存储、libxml2 订阅解析和 libcurl 联网；桌面使用 SDL 展示相同的渲染结果。不是 KOReader 插件，也不是网页套壳。

**当前版本为 v0.0.4-beta.4，尚未完成整体验收。** 本版保留 OPML 源库、订阅任务首页及升级进度反馈，并修复 BusyBox timeout 监控进程继承锁导致的升级回滚。见 [beta.4 说明](RELEASE-v0.0.4-beta.4.md)和[旧版启动器迁移](INSTALL.md#旧版升级前修复启动器)。[beta 安装包](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.4-beta.4)提供中文字体和证书。KPW3 已有锁故障复现、热修后 beta.3 启动与数据库逐字节一致的证据；本版安装包、其他机型、休眠和断电恢复的验证范围分别记录，不能用历史实测代替。完整范围见[实施计划](PLAN.md)，历史设备证据见 [DEVICE.md](DEVICE.md)。

## 安装到 Kindle

使用 Release 附件安装，无需本机构建。请先阅读 [安装与升级教程](INSTALL.md)，确认运行环境，再合并复制安装包；升级时保留 `native-rss/data/`。

## 本机构建

需要 CMake、pkg-config、C++17 编译器及 libxml2、sqlite3 3.38+（启用内置 JSON 函数）、libcurl（支持 CURLOPT_PROTOCOLS_STR）、OpenSSL、zlib、freetype2、HarfBuzz、libpng、libwebp、libavif 1.1.0+（启用 libyuv）、sdl2 开发库。

先确认这些库能通过 `pkg-config` 查到；非系统前缀安装时，将对应的 `lib/pkgconfig` 加入 `PKG_CONFIG_PATH`。例如 libavif 安装在自选前缀后，使用 `PKG_CONFIG_PATH=/absolute/prefix/lib/pkgconfig`。不要将 ARM 的依赖目录用于宿主构建。缺少 `libavif` 时 CMake 会停止；已有旧构建目录成功不代表新环境能直接构建。

```sh
rtk proxy python3 native-rss/tools/build_reader.py host
rtk proxy ctest --test-dir native-rss/build --output-on-failure
```

默认构建启用 CREngine。`tools/reader-sources.json` 固定上游版本与归档校验；`tools/build_reader.py host` 下载并校验源码、在 `build-host-reader/prefix` 准备依赖后构建正式应用。无需通过 experiments 目录构建。需要单独验证原渲染器时，配置 `-DRSS_CRE=OFF`；`RSS_CRE_SOURCE` 可显式指定已校验源码位置。

从本仓库根目录运行。测试使用临时数据库与本机 HTTP 服务，不接触 Kindle。Linux 的中文视觉检查需要 `RSS_FONT` 指向合适字体；测试 fallback 字体不代表中文显示验收。

上面的 `rtk proxy` 是本仓库使用的命令代理；没有安装 RTK 时可直接运行其后的命令。仅构建命令行与测试、不需要 SDL 窗口时，可在配置命令追加 `-DRSS_DESKTOP=OFF`。

## 网页正文

`src/fulltext.cpp` 使用已有 libxml2 提取并清洗正文，不引入 JavaScript 运行时或网络提取服务。仅接受 UTF-8 / ASCII，输入上限 8 MiB、输出 HTML 和文本各 512 KiB、DOM 最多 10000 节点 / 64 层。语义正文与普通容器按文本和链接密度选择；裸 body 页面要求多段正文证据。提取结果不保证覆盖所有站点，登录、验证码、旧编码和脚本生成页面应回退到 RSS 原文。

`article_fulltext` 附表保存 pending / ready / failed、正文、最终网页地址及独立位置，`article_fulltext_images` 保存图片顺序，复用 `article_images` 像素缓存。原 `entries.html/text/position` 继续属于 RSS 原文。正文选择使用 `reader_content:<id>`；网页正文的排版书签使用 `reader_cre:web:<id>`，原有 `reader_cre:<id>` 不变。订阅自动下载使用 `feed_fulltext:<id>`，默认关闭且只在新增文章时入队。

正文和派生图片请求逐次验证跳转及目标地址，禁止非 HTTP(S)、账号密码 URL 及未配置的内网目标。显式订阅地址的同 origin 内网资源仍可读取；派生请求不使用环境代理，以免代理绕过目标地址校验。正文每轮最多 10 篇，单篇请求最多 15 秒、阶段预算 120 秒；剩余 pending 在后续同步（包含 RSS 304）继续，failed 经手动重试重新入队。单篇下载复用 UI 后台线程，不在绘制或数据库事务中联网。

本地临时数据库可使用 `kindle-rss --db /tmp/test.sqlite fulltext ID` 验证单篇下载，`sync failed` 重试失败项目。`fulltext`、`fulltext_app`、`store`、`net` 和本机 HTTP `network` 测试覆盖提取、迁移、正文切换、取消和失败路径；宿主截图与 ARM 编译不等于真机内存、触摸或耗电验收。

## 文章翻译

`src/translation.*` 用现有 libxml2 将正文拆成互不重叠的翻译段落，保留外层文章结构；图片和代码使用受控占位符保护。单段正文不超过 6000 UTF-8 字节。翻译请求保留完整语义段落，去掉行内链接/强调标签后紧密拼接文字，避免把单词、介词短语拆散。代码/图片用受控占位符保护；Qwen 请求还在 `terms` 中加入同源同译术语表，模型看不到真实代码或图片 HTML。所有服务返回的纯文字必须通过 UTF-8、大小、无新增标签和占位符校验。原文保留链接与强调结构；译文按中文语序呈现，不强求沿用英文链接位置。`Entry.reading_mode` 区分原文、译文、双语，`web_content` 继续区分 RSS / 网页来源。双语由段落缓存拼装，生成后重新派生纯文本，供两种排版复用。

`src/qwen.*` 保留现有翻译服务实现入口，并读取数据库同目录下的 `translation.conf`：`RSS_TRANSLATION_PROVIDER`（默认 `qwen`）、`RSS_TRANSLATION_API_KEY`、`RSS_TRANSLATION_MODEL`、`RSS_TRANSLATION_ENDPOINT`。内置 provider 为 `qwen`、`siliconflow`、`zhipu`，各自默认模型和完整 endpoint 见[使用指南](USAGE.md#配置翻译服务)；`openai-compatible` 要求显式指定 model 和 endpoint。Qwen provider 仅支持 `qwen-mt-` 系列模型。配置最多 16 KiB，使用简单键值格式，不执行 shell；每次任务重读全部配置。所有服务商共用这四个配置项，不读取服务商专用变量；同名环境变量优先于文件，空值也视为已设置。密钥不进入 SQLite、OPML、安装包或源码包。外部地址只接受 HTTPS，HTTP 仅允许字面回环地址供本机测试；带鉴权的请求不跟随重定向、不继承代理，不回显服务端错误正文。

统一网络入口复用 `net.cpp`、UI 的单后台任务与取消信号，翻译不在绘制或数据库事务内联网。单请求超时 45 秒、响应最多 256 KiB，逐次请求间隔至少 1.1 秒，一轮预算约五分钟；未完成时暂停，只有用户重试才继续，不自动重试可能已计费的请求。Qwen 请求使用单条 user 消息、`translation_options` 指定 English → Chinese，并以 `terms` 保护受控标记；其他兼容服务使用包含翻译要求的单条 user 消息，标记仍受保护。要求模型输出中文纯文本；检查 `finish_reason`，截断结果不缓存，并执行相同的响应与占位符校验。JSON 解码复用 SQLite 3.38+ 内置 JSON 函数，不新增 SDK 或第三方解析器。

`article_translations` 与 `article_translation_blocks` 附表以文章、来源及源内容指纹区分缓存，不按服务商分桶。切换 provider 不自动重译或清缓存；继续未完成文章时可能复用先前服务商生成的段落。每段单独事务提交；失败或取消保留成功段落，新版本完成前保留旧版本缓存，完整保存新版本后再清理旧版本。整段纯文本缓存使用 `rss-translation-v3` 指纹前缀，旧 v2 碎片缓存不会继续用于显示，需手动重新翻译。文章删除时级联清理，收藏随原文保留。翻译模式的阅读偏好与书签使用独立设置键；原来的 RSS / 网页原文位置不被译文覆盖。跨模式用正文 Unicode 字符位置和段落映射定位，不沿用不同文档的页码。额外保存当前文档对应的源指纹、已缓存段落数及原文段落位置；后台补全译文后据此恢复，缓存未变时仍使用精确书签。只读预览不写入恢复位置，原文更新后不套用旧段落位置。双语模式的嵌套图片只保留在原文一侧，译文模式继续保留图片。新版排版将原文缩小并降低视觉权重，中文使用正常字重和完整宽度，取消译文侧竖线、缩进和密集链接下划线；行距继续跟随用户设置，原文模式样式不变。

CLI 可在临时数据库执行 `translate ID` 或 `translate-web ID`。`translation`、`qwen`、`translation_app` 和本机模拟服务 `translation_network` 覆盖段落、配置、缓存、取消、失败、缓存复用、模式切换及旧数据保护；不消耗真实翻译额度。1072×1448 模拟截图通过 `RSS_TEST_SNAPSHOTS` 输出。外部 API 的真实译文质量、KPW3 内存、触摸和耗电仍需另行验证。

`selection.hpp` 定义选区、任务快照与存储记录；`app_selection.cpp` 实现仅保留翻译按钮的选词工具条及选区译文分页。CREngine 用真实文字命中与分行矩形建立 DOM 范围锚点，选区指纹不含来源元信息或排版参数；原排版使用正文 UTF-8 字节范围和实际字宽定位。回绘会核对内容指纹和原文片段，范围按来源与阅读模式隔离，不把文字范围替换为页码。划线在本应用中显示，不接入原生系统笔记。

Store 通过事务内 `CREATE TABLE IF NOT EXISTS article_selections` 增加附表，保留原有数据。复合主键包含文章、来源、模式、指纹和起止锚点；独立更新划线开关或译文，不互相覆盖，删除文章时级联清理。正文更新保留旧行但不再投影；取消订阅时收藏保留规则不变。选中文字最多 6000 UTF-8 字节，译文最多 64 KiB；所有 SQL 使用绑定参数。选区翻译复用唯一后台 future、当前翻译服务配置及相同的 HTTP 安全边界，用户点击才请求；任务快照隔离页面切换后的晚到结果，取消和错误不自动重试。触摸解码器以单调时钟区分 600ms 长按、拖动、完成和取消，拖动事件按批合并；Kindle 和 SDL 共用解码路径。

## ARM 构建状态

Apple Silicon macOS 上的依赖构建脚本为 `tools/build_arm.py`，依赖版本与校验来源在 `tools/arm-sources.json`。需要 CMake 3.22+、Make、Perl、curl 和 Python 3.12+，以及 `vendor/zig-aarch64-macos-0.15.2/zig`。Zig 官方归档为 `https://ziglang.org/download/0.15.2/zig-aarch64-macos-0.15.2.tar.xz`，SHA-256 为 `3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b`。编译器目前需预先放置在该位置。

```sh
rtk proxy python3 native-rss/tools/build_arm.py
```

`build_arm.py` 在基础依赖之后调用 `build_reader.py` 的同一构建流程，默认链接固定版本的 CREngine-ng；`BUILD.json` 同时记录引擎提交和包含构建脚本/CMake/依赖锁的输入指纹。

产物为 `build-arm/kindle-rss`，目标 ARMv7 Cortex-A9、EABI5 soft-float、静态链接。脚本检查 ELF 类型、浮点 ABI、程序段边界及动态链接段；这些检查不证明旧内核兼容性。已在 KPW3 5.16.2.1.1 运行并验证图文渲染。设备端 `device` 运行循环与 `device/launch.sh` 会话启动器已接通；不要绕过启动器的入口检查。KUAL 入口位于 `device/kual/`，仍非完成验收的安装包。

ARM 网络访问使用开发包中的 `/mnt/us/native-rss/certs/ca-bundle.pem`，也可通过 `RSS_CA_BUNDLE` 指定。不能依赖构建主机上的 OpenSSL 证书路径。

## 生成开发包

```sh
rtk proxy python3 native-rss/tools/package.py
```

需要先完成 ARM 构建。工具验证源码及二进制哈希，拒绝过期产物；缺少运行资源时按 `tools/runtime-sources.json` 下载并验证。输出 `output/kindle-rss-kindle-dev.zip` 和 `output/kindle-rss-source-dev.zip`，不自动安装。包含 Noto Sans SC（OFL 1.1）、Mozilla 根证书（MPL 2.0）、KUAL 入口、第三方许可、安装说明和逐文件 SHA-256 清单。程序使用未修改的可变字体，设置字重为 400。

组合阅读程序选择 GPLv3 分发，原始源码保留 MIT；CREngine-ng 的固定版本及上游许可声明必须与已核对的来源记录一致。`release.py` 同时验证许可、构建输入、安装文件、项目源码和第三方对应源码，任何一项缺失或不匹配都会拒绝发布。详见[许可与源码核对](RELEASE_LICENSE_AUDIT.md)。

项目自身的 MIT 许可与第三方组件汇总同时放入安装包；源码包还包含仓库首页、贡献指南、安全反馈和同一组第三方许可。源码按文件类型白名单收集，诊断 TXT、日志、数据库与 OPML 不属于打包输入。发布前仍需核对实际 ZIP 内容，不能只依赖 `.gitignore`。

## GitHub Release

### 在线升级协议

`src/update.cpp` 使用已有 libcurl、SQLite JSON、OpenSSL SHA256 与 zlib。正式版手动查询固定公开仓库的 `/releases/latest`，只接受数值更大的 `vMAJOR.MINOR.PATCH` 正式版本；`vMAJOR.MINOR.PATCH-beta.N` 客户端查询 `/releases?per_page=100`，从最近 100 条发布中选择更高的 beta 或正式版。同一基础版本按 beta 序号递增，正式版高于所有 beta；拒绝草稿、同版本、降级和与标签不匹配的预发布标记，不安装主分支提交。按文件名选择 Kindle 兼容 ZIP 与 `SHA256SUMS.txt`；不发送 GitHub 令牌，初始资产地址必须属于本仓库对应标签。每次跳转重新校验 HTTPS 与 GitHub 域名，禁止降级 HTTP 或跳出 GitHub 服务。API、校验文本、ZIP 分别限制为 2 MiB、64 KiB、128 MiB；解压后最多 256 MiB / 1024 文件。联网走 UI 单后台任务和取消信号，不与订阅同步或翻译并行。

`package.py` 将 `native-rss/UPDATE.json`（`format: 1`、`model: Kindle`、CMake 对应版本，含 beta 后缀）纳入逐文件 MANIFEST。`release.py` 校验元数据与标签一致，安装附件使用 `RSS-Reader-<版本标签>-Kindle.zip`，例如 `RSS-Reader-v0.0.4-beta.4-Kindle.zip`。历史 KPW3 专用开发版需经 USB 迁移一次，兼容版不会在线回退到旧专用包。升级器仅解压本打包器生成的普通 DEFLATE ZIP 条目，拒绝 ZIP64、加密、数据描述符、额外字段、重复或越界路径、个人数据路径及目标符号链接，并核对逐文件哈希与实际文件集合。未来更改归档特性或协议时需要先升级客户端，不应将不兼容的包公开为最新版。

下载暂存于 `native-rss/.update`；安装前通过 SQLite backup API 保存一致性数据库副本。阅读器以专用退出码 42 交接到旧程序副本 `runner`：启动器等待数据库关闭，在恢复监护与暂停 volumd 的保护下执行 `update-apply`。安装先完整备份现有文件并持久化 journal，再逐文件以 rename 替换；不原地覆盖运行中的文件，不整目录替换 data/documents/extensions。新的启动器恢复系统界面后重启，程序首次成功显示设备画面才提交 journal。成功安装并完成系统恢复后先显式 `flock -u 9`，再关闭描述符并 exec 新启动器；BusyBox timeout 的监控子进程可能仍持有继承的 FD，仅关闭父进程的 FD 不足以释放锁。旧客户端应先更新启动器并重开，目标包不能修复旧进程。安装出错立即回退，安装中断或新程序启动失败则在下次启动、打开数据库之前通过旧 runner 恢复。此机制不提供数据库迁移功能，也不证明硬件掉电时文件系统仍完整；外部备份和真机验收仍必要。

`kindle-rss check-update` 可只读检查真实 GitHub；`update-apply` / `update-recover` 是启动器内部命令，要求启动器保护环境，前者绑定 Linux 父进程生命周期。`update`、`update_archive`、`update_network`、`update_app` 与 `launcher` 覆盖版本、真实 ZIP、临时目录安装回退、SQLite、模拟 GitHub TLS 下载与跳转、取消和 1072×1448 界面状态。网络测试使用临时自签证书和本机代理，不接触真实 Release 附件；`RSS_TEST_SNAPSHOTS` 可输出升级界面 PGM。测试不安装或操作 Kindle。

### 发布步骤

当前发布说明见 [beta.4](RELEASE-v0.0.4-beta.4.md)，历史记录见 [beta.1](RELEASE-v0.0.4-beta.1.md)、[beta.2](RELEASE-v0.0.4-beta.2.md) 与 [beta.3](RELEASE-v0.0.4-beta.3.md)，正式版记录见 [v0.0.3](RELEASE-v0.0.3.md)。更新 CMake 基础版本、`RSS_VERSION_SUFFIX`、KUAL 版本号及对应发布文档后提交，再推送 `v数字.数字.数字` 或 `v数字.数字.数字-beta.序号` 标签，触发[发布工作流](../../.github/workflows/release.yml)。安装文档分别链接正式版最新版和 beta 标签页，不能用 `/releases/latest` 代替 beta 下载入口。

工作流使用 Apple Silicon macOS runner、固定 Zig 哈希，运行宿主 CTest、重编译 ARM，再运行 `tools/release.py`。后者生成安装包、项目源码、第三方对应源码和 `SHA256SUMS.txt`。第三方归档按构建锁验证，项目源码必须与二进制的构建输入一致；仅修改 `public_release_ready` 布尔值无法通过检查。

通过检查后创建或更新同名 Release 草稿，上传四个附件；beta 标签设置预发布标记，已公开的版本不会被覆盖。发布说明由 `tools/release_notes.py` 读取对应版本文档并将相对链接转换为固定提交链接。下载附件、核对校验值并完成 KPW3 安装包验证后，再手动公开草稿；beta 保持预发布，正式版发布时再按需要标为最新版。工作流不会自动部署设备或提前公开附件。

已发布附件若仅修订文档，需保留原程序、非文档源码和 `BUILD.json`，在 `native-rss/DOCUMENTATION.json` 中记录应用提交与文档修订提交，并重算安装清单及附件校验值。发布说明同时标注文档更新，不能把文档修订称为重新编译。GitHub 自动生成的源码归档仍对应原版本标签，后补的文档以自定义源码包及其文档修订记录为准。

FreeType 按 FreeType License 使用。Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.

## 在电脑上运行

Kindle 上的按钮操作见[使用指南](USAGE.md)。下面的命令用于电脑上的开发与验证。

```sh
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite import /absolute/path/feeds.opml
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite sync
rtk proxy native-rss/build/kindle-rss --db native-rss/output/rss.sqlite desktop
```

桌面窗口支持点击、左右方向键翻页、Esc 返回、系统输入法及 Cmd/Ctrl+V 粘贴。点击左下角「菜单」中的「同步」启动同步并显示进度弹窗；可取消同步，或点「后台运行」收起弹窗继续浏览文章，任务继续执行。再次进入只查看现有任务，不会重复启动。失败后弹窗保留失败源分页、编辑和重试入口。新内容不会自动重排当前列表；点击更新提示刷新。

GUI 的导入按钮读取数据库同目录下的 `feeds.opml`；导出写入 `subscriptions.opml`。CLI 的 `import` 和 `export` 可指定其他路径。OPML 仅含订阅与分组，不包含阅读状态或正文。

点击首页左下角「菜单」展开「订阅 / 设置 / 同步 / 退出」。从订阅管理进入「我的订阅」，按分组展示来源，点击来源名称阅读该源文章，点击右侧「编辑」修改名称、RSS 地址或分组；名称可留空，同步成功后自动读取博客名称。修改仅在右上角「保存」时写入，放弃或返回会丢弃草稿。同步中的订阅地址需要先取消同步再修改。取消订阅需要确认，只保留收藏文章；后台已开始的下载不会恢复已取消订阅的文章。

首页大标题旁的下拉箭头可切换阅读范围，按名称或完整路径查找分组；父分组包含子分组，例如「科技」包含「科技/编程」，不包含「科技新闻」。列表显示来源、所属分组和日期，收藏只显示标题左侧的实心星标。未读、全部、收藏和文章搜索保留当前阅读范围；「退出搜索」或提交空关键词退出搜索。

订阅管理首页的「管理分组」可新增、重命名、移动或删除多级分组。点击「子分组」在当前组内创建；新增和编辑时选择「上级分组」，选择「顶层（无上级）」可移回根目录。更换上级会移动整棵子树，保存前仅修改草稿；上级选择支持按完整路径查找，并排除自身及后代。空分组会保留，重名目标不覆盖；删除分组及其子分组时把组内订阅移到「未分组」，保留文章、收藏和阅读状态。订阅管理首页的「导入与导出」用于迁移订阅及空分组。「缓存管理」从同步弹窗进入，可确认清理 30 天前的已读文章，保留收藏、未读及正在阅读的文章。清理会删除这些文章及状态；订阅源仍提供的文章可能在下次同步后重新出现。原生软键盘支持拼音选字和词组，桌面仍可使用系统输入法。

`--font /absolute/path/font.ttf` 或 `RSS_FONT` 可以指定字体。macOS 默认使用本机系统字体，仅本机读取，不随项目分发。

列表左下角「菜单」向上展开应用菜单，菜单顶部显示 `RSS Reader`，提供订阅、设置、同步与退出；同步不再占用顶部右侧，点击菜单中的同步仍直接启动任务，运行中进入只显示现有进度。首页时间靠左，与范围标题使用同一左边距；带返回按钮的二级页面时间保持居中，阅读页时间保留在底部左侧，Wi-Fi 和电量靠右。

顶部时间与电量使用独立的界面字体：设备已有 `/mnt/us/fonts/AlibabaPuHuiTi-3-55-Regular.ttf` 时直接读取；缺失或加载失败时回退到应用字体，不影响启动。该可选字体不随安装包分发，正文仍使用原来的字体。状态栏由本应用绘制并读取系统实时状态；启动器仍暂停原生状态栏，避免争用 framebuffer。

## 界面约定

首页采用单内容标题：顶部只显示左侧时间和右侧设备状态，不再常驻应用名，也不绘制状态栏下的分隔线。范围与筛选行整体上移 24 px，基线 y124，分隔线和列表起点 y164；搜索时的列表起点为 y252。该行将 40 px 范围标题、未读/全部/收藏和搜索合并：标题显示“全部订阅”、分组名或订阅源名，点击打开分组选择；筛选和搜索保留当前范围。长标题截断并为箭头保留空间，搜索图标使用独立触控区。搜索结果在下一行显示关键词和退出入口。页脚左侧放「菜单」，中间显示文章数量与页码，右侧并排放「上页 / 下页」。数量与页码过长时分成两行，避免挤占翻页按钮；首尾页对应的翻页按钮置灰并禁用。首页底栏触控区域均高 88 px，菜单为 176 px 宽，左右翻页分别为 148/140 px 宽，中间留 24 px 间隔。

顶部状态栏共用 `device_status`：固定字号 31 px、顶部参数 17 px、基线 y48，Wi-Fi 40 px、电池 48 px；阅读页右上角设备状态使用首页相同字号与顶部距离。各页面时间和返回按钮保持原位：普通页面返回图标 40 px、触控范围 x24/y6/96×64，分隔线 y76，标题 x32/y154；阅读页返回触控范围 x24/y8/96×88，收藏仍在右上角，展开工具时按原行为替换设备状态。阅读页时间在底部 x32、基线 h−20，字号统一为 31 px。添加、查找仍与标题同行，正文分页与逐层返回行为不变。搜索与编辑输入页沿用同一页头，候选区下移 50 px，底部键盘位置不变。

按角色统一字号：页面标题 48 px；弹窗和空状态标题 36 px；分组、订阅、表单值和键帽 32 px；常用操作与面板正文 28 px；辅助说明、元信息和分页计数 24 px。文章列表常规行高 160 px，按标题与来源信息的实际字形高度上下居中；KPW3 普通列表通常每页容纳 7 篇，多行标题按需增高。文章列表标题 36 px、阅读正文可调 24–48 px，属于内容排版，不与界面操作文字强制等大。底部操作区使用 88 px 控件和距底部 104 px 的分隔线；阅读工具栏仍为 96 px。

按钮按实际字形高度垂直居中，主要操作使用黑底白字。页面横向边距 32 px；分区线使用灰度 150，列表和页脚分隔线使用 205，均为 1 px。输入框为 1 px 灰边，弹窗为 2 px 深色边；居中弹窗横向留白 48 px、内边距 32 px，并淡化底下的内容。空状态使用统一线性图标、简短说明和 88 px 高的操作按钮；搜索词已在搜索栏呈现，空结果提示不再重复整段关键词。

`reader_layout_test` 检查各页返回区域像素一致，并实际点击新坐标验证输入取消、搜索范围返回、分组草稿、确认取消与错误返回；`app_test` 覆盖下移后的拼音候选和取消输入。设置面板的开合仍不改变正文分页或锚点。

自动同步使用现有 `settings` 键 `auto_sync`（缺省开启，`0` 关闭）和 `auto_sync_last_attempt`（最近一次手动或自动 RSS 同步尝试的 Unix 时间）；不做数据库 schema 迁移。调度由 UI 主循环复用唯一同步 future，每 30 秒检查当前 Wi-Fi 状态，尝试间隔固定 1800 秒；最近的有效订阅 `synced_at` 与保存的尝试时间共同节流，失败或取消也保留尝试时间。准备自动任务时的存储错误只记录日志并延后 30 分钟再检查，手动任务准备失败会释放运行状态并保持原错误提示流程。休眠时应用不运行，因此不会唤醒设备补做任务。文章列表或正文阅读时可后台启动，设置、输入与确认等交互时推迟；列表数据延迟至安全绘制时刷新，完成事件不覆盖用户操作产生的重绘标记。`auto_sync_test` 以确定时间调用真实 App 行为，不启动网络 worker。

## 渲染与示例

示例只在显式调用 `demo` 时写入，不伪装成真实订阅。使用独立数据库，避免混入个人订阅。

```sh
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite demo
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite render native-rss/output/list.pgm
rtk proxy native-rss/build/kindle-rss --db native-rss/output/demo.sqlite --article 1 render native-rss/output/reader.pgm
```

生成 1072×1448 的 PGM 灰度图，采用与应用相同的布局代码。渲染命令不改变文章已读状态。

## 性能验证

常用图标按类型和尺寸缓存 16 位覆盖率，复用时直接做整数灰度混合，不再重复计算曲线距离。缓存不依赖位置、颜色或背景；每个 Canvas 的覆盖率数据上限 128 KiB，超限清空重建，超过 128×128 的图标沿用直接绘制。状态栏独立画布拥有自己的缓存。

`rss-performance` 使用独立内存数据库，测量查询、列表翻页、菜单、搜索、正文与图标绘制，不连接真实订阅库，也不操作 framebuffer。页面切换包含状态断言，避免布局变化后点击落空却继续报出误导数据。

```sh
rtk cmake --build native-rss/build --target rss-performance -j 4
rtk proxy native-rss/build/rss-performance native-rss/vendor/runtime/reader.ttf 100
rtk proxy native-rss/build/rss-performance native-rss/vendor/runtime/reader.ttf 10000
```

设置 `RSS_BENCH_SNAPSHOTS` 可保存关键页面的 PGM，供优化前后比较。比较时使用相同字体、文章数量、构建配置及设备，区分首次绘制与缓存命中；输出是 CPU 计算与绘制时间，不包含墨水屏刷新。桌面耗时不能代表 Kindle，涉及渲染的优化需补 ARM 真机对照。

## 当前行为与限制

- RSS 2/RDF/Atom、嵌套 OPML 导入导出；导入按 URL 去重，保留已有名称和分组。
- 同步使用 ETag/Last-Modified；TLS 校验开启；单源失败不删除旧数据，不阻止其他源更新。
- 单份订阅限制 8 MiB，连接超时 10 秒，总请求超时 35 秒。
- 默认正文使用 CREngine，原排版仍可在面板切回；独立书签存入 `reader_cre:<ID>`，原版 `entries.position` 保留。未设置过引擎且已有旧进度的文章先沿用原排版。HTML剥离源属性、脚本及外部样式，仅加载受控CSS和缓存图片；最多512KiB正文、512页、200图片节点及16MiB图片预算，失败提示并回退。
- 原排版按原文顺序图文分页：图片等比缩放，放不下时整张移到下一页；短 figcaption 与图片保持同页，较长图注继续分页。点击正文图片可打开对应大图，返回保持当前页。保留旧文字阅读位置，额外记录连续图片页的位置。未做通用网页全文提取，不能保证摘要源的全文离线。
- 同步保留未改变的图片引用行，只有说明、排序或引用集合变化时才更新，避免重复改写已缓存的像素 BLOB、长时间阻塞前台阅读。
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

通过 Wi-Fi SSH 调试时，USBNetwork 应配置 `USE_WIFI="true"`、`USE_WIFI_SSHD_ONLY="true"`，避免启用 SSH 时把 USB 切为网卡模式。退出阅读器后若只能充电，先核对会话日志、`volumd` 进程状态与 `useUsbForNetwork`；`useUsbForNetwork=1` 且加载 `g_ether` 表示 USBNetwork 模式，不能据此断言阅读器未恢复服务。切回 USBMS 后应为 `useUsbForNetwork=0`、加载 `g_file_storage`，再通过实际拔插和主机磁盘挂载确认。不要让阅读器启动器强制覆盖用户的 USBNetwork 模式。

这一节供定位启动失败时核对，不需要让安装用户逐项执行或记住命令。先按真实调用链区分入口问题与运行环境问题：KUAL 菜单和 `reader-scriptlet.sh` 最终都调用 `launch.sh`，后者只在选择书库入口时需要 SH_Integration。`launch.sh` 不调用或检查 Hotfix、Scriptlets 或商店；它单独检查并调用 FBInk。Universal Hotfix 可以提供 Scriptlets 和 FBInk，但不是唯一来源，不应无条件要求安装或重装。用户侧按[安装教程](INSTALL.md)复制并打开应用，只有入口缺失或启动失败时才进入[启动问题排查](TROUBLESHOOTING.md)，按对应现象检查环境或导出检查文件。

启动器在 guardian fork 前创建 `/tmp/rss-cre-session.*` 私有目录并导出 `RSS_CRE_CACHE`。退出时先关闭阅读器并删除本会话缓存，再恢复服务及USB；启动器被杀时由独立监护进程执行同样的清理。原会话锁、数据库路径及系统恢复保护继续使用，不需要额外测试入口。

启动器的实际条件以 `device/launch.sh` 为准：需要一份通过设备预检的 FBInk，以及 `timeout`、`flock`、`stat`、`pidof`、`awk`、`dd`、`mktemp`、`mkfifo`、`head` 和系统 LIPC 工具；设备密码锁开启时会拒绝启动。依次尝试 `/mnt/us/libkh/bin/fbink`、`/usr/bin/fbink`、`/mnt/us/koreader/fbink`，通过 `RSS_FBINK` 将所选路径传给阅读器，保证预检、刷新和恢复一致；不修改已有工具。真机历史证据仍只来自 KPW3；兼容版使用动态屏幕快照与 FBInk 刷新，屏幕和触摸能力检查不应绕过。

存在 `/etc/upstart/statusbar.conf` 时还需要 `/sbin/initctl`：只读取任务状态和主进程 PID，将其纳入已有的进程身份、暂停和恢复保护，不执行任务 stop/start。预先停止的状态栏保持停止，身份不明或阅读期间发生替换会退出阅读。Snow 面板的选择和硬件修订回退见 [机型兼容说明](COMPATIBILITY.md)。

`/etc/prettyversion.txt` 的 Kindle 5 版本决定 Pillow 恢复策略，5.6.5 前软隐藏／显示，之后硬禁用／启用；未知版本与 SysV 在接管前拒绝。机型表只提供 DPI、平台提示和明确的触摸例外，不替代硬件检查或首页固件哈希校验。电量首选有时间与长度上限的 LIPC 查询，再逐项回退 sysfs；现有 30 秒采样频率保持不变。退出时 `display_metrics` 汇总缩放、差分、复制和 FBInk 耗时，测量方法及验证边界见 [机型兼容说明](COMPATIBILITY.md)。

`process.cpp` 为每个外部命令创建独立监护，负责命令组超时和阅读器消失后的清理；监护不保留其他命令的生命周期管道。会话 FIFO 随实际工具退出而关闭。`capture_bounded` 默认只捕获 stdout，刷新调用额外合并 stderr 并严格校验，不能只凭 FBInk 的成功退出码提交 `last`。`process_test` 覆盖阅读器 SIGKILL、持 FIFO 后代、超时、后台退出和标准描述符缺失；`device_test` 覆盖所有 32 个 Type B 主槽、逐槽增量坐标、多指取消，以及退出 0 的刷新等待错误。

`device/diagnose.sh` 导出部分工具路径、设备参数和启动日志，但不会自动补装组件，也不会逐项覆盖启动器的全部检查。协助用户时先根据报告确认原因，再提供适合其设备的补装步骤。FBInk 的上游来源见[第三方组件说明](../../THIRD_PARTY_NOTICES.md)，程序本身不包含在当前安装包中。

### 随包资源与共享启动工具

- 正文所需字体与证书随 RSS 安装包提供；顶部状态栏可复用设备 `/mnt/us/fonts` 中已有的界面字体，缺失时自动回退。
- Scriptlets 是可选的共享书库启动能力；书库 `.sh` 入口通过 SH_Integration 启动，KUAL 入口不经过它。
- `# DontUseFBInk` 只禁止 SH_Integration 把脚本输出额外绘制到屏幕，不能据此认为 RSS 不需要 FBInk：`launch.sh` 仍在恢复系统画面时执行它。

## 设备调试空间与写入失败恢复（2026-10-30）

真机验证结束后，应清理本次临时数据库、截图和测试副本；旧程序备份归档到电脑并校验后移出 Kindle，仅保留最近一次可用的回滚版本。不要累积 `native-rss-qa` 和历史应用备份。原生首页恢复文件、用户数据库和图书不在调试清理范围内。部署前检查用户分区及临时分区，预计部署和测试结束后至少保留 128 MiB 用户分区空间；不足时先归档自己的调试产物。该阈值是开发流程的最低余量，不保证未来同步不会继续耗尽空间。

数据库写入异常不能阻断退出：打开文章通过 `Store::begin_reading` 在同一事务内提交已读、进度、正文选择和最近阅读记录；任一步或最终提交失败都回滚，并清除未完成的阅读界面，关闭错误提示回到列表。退出文章保存进度失败时仍返回列表，并明确提示本次进度未保存。`app_test` 使用 SQLite 触发器在真实写入调用处注入错误，覆盖新版与原排版的首条及后续写入失败、未读状态保留、错误返回、旧书签保留和恢复后重新阅读；`store_test` 还验证网页正文进度与延迟约束导致的提交失败回滚。此测试验证错误处理，不假装是物理磁盘满载测试。
