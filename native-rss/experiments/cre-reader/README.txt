RSS / CREngine 隔离排版样机

目的
在同一组虚构文章上比较现有 RSS 正文渲染与 KOReader CREngine。
最初阶段是隔离样机；下方“应用内实验接入”记录后续可选集成。两种实验均不替换正式设备安装包，不提供查词或笔记 UI。

实验输入与布局
- make_fixtures.py 生成中文长文、中英混排、引用代码、多图文章，及两张确定性灰度 PNG。
- 两边使用项目现有 vendor/runtime/reader.ttf、32px 正文字号、1072x1448 画布。
- 正文边距左右64px、上下84px；现有渲染行高52px，CREngine 使用素材 CSS 的162.5%行高。
- baseline.cpp 直接编译现有 src/feed.cpp、src/render.cpp；不改正式代码。
- 基线的两张灰度测试 PNG 使用已为 CREngine 构建的 libpng 读取，避免引入全部生产图像解码器。
- CREngine 加载上游 epub.css 及其 html5.css 导入，再应用素材内的少量语义样式。
- 这是正文排版对照，没有复制正式应用的常驻工具栏；不能将输出当作已完成的产品界面。

复现（从仓库根目录运行）
rtk proxy python3 native-rss/experiments/cre-reader/build.py host
rtk proxy python3 native-rss/experiments/cre-reader/check.py
rtk proxy python3 native-rss/experiments/cre-reader/build.py arm

ARM 构建复用本仓库已有的 Zig 和 build-arm/prefix 静态依赖，要求它们已准备好。
新依赖安装到 output/cre-reader-20261027/arm-prefix，不修改正式构建目录。
host 使用已安装的 FreeType、HarfBuzz、libpng、libxml2；xxHash 和 libunibreak 单独构建。
sources.json 固定上游版本与下载归档 SHA-256，下载、解包与 ELF 验证复用 tools/build_arm.py。
构建输出均位于已被 .gitignore 排除的 output/、vendor/，不加入发布包。

程序
baseline FONT HTML OUTDIR
cre-probe FONT HTML CSS OUTDIR

双方输出 page-001.pgm 等完整分页，以及标准输出的一行 JSON 指标。
cre-probe 还输出 anchor.txt，验证从文件读回书签、重开恢复、字号32改40后定位、坐标命中文字。
图片测试验证本地图片资源可加载；这不等同于实现拖动选词或查词菜单。
PNG 的图注跟图分页依靠 figure/figcaption 的 CSS；仅换引擎而不配置阅读样式无法保证效果。

指标边界
- layout_ms 包含字体初始化、解析和分页；基线另外包含测试图像解码。
- render_total_ms 包含各页绘制与像素输出准备，不含PGM磁盘写入。
- peak_rss_kib 为进程级峰值，CREngine 在重开/改字号自检之前取样。
- selfcheck_peak_rss_kib 另外记录 CREngine 自检后的峰值。
- 指标不含 framebuffer 刷新、触摸、SQLite 或网络；宿主结果不能当作 Kindle 性能。
- 基线和 CREngine 的图像解码阶段不同，多图场景的阶段耗时不能严格逐项相减。

验证
check.py 独立生成素材，核对8组完整渲染、页面尺寸、书签与图片结果，验证缺失/空/超限输入被拒绝。
page-preview 另有启动器保护检查。两个 ARM 计算程序和预览程序均检查静态 ARM EABI5 soft-float ELF。
此处为隔离样机的验证范围；应用内集成的全库验证见后文。

真机运行边界
只把计算程序、CSS 和素材复制到独立测试目录；设置内存与执行时间上限，不打开RSS数据库。
page-preview 用于展示一张预生成的PGM，必须从原有 launch.sh 的隔离副本启动：
改 APP 为实验 page-preview，STATE 为 /var/local/native-rss-cre-probe，DB 为实验目录中的未使用路径；
通过 RSS_CRE_PREVIEW 指定页面。不得更改正式启动器或直接绕过会话启动它。
展示期间任意触摸退出，原有启动器负责屏幕、服务及USB恢复。
预览只是显示已渲染结果，不能用它的翻页速度代替引擎实际绘制速度。

当前结果与原始证据
见 output/cre-reader-20261027/host-metrics.json、host-results/、各构建日志及该目录中的验证记录。
实机结果只有在设备运行后才能补充；不能以ARM构建成功代表真机验收。

2026-10-27 真机结果
KPW3 / 5.16.2.1.1，双方字体 SHA-256 相同；四组素材的8次计算均运行通过。
单次测量：CREngine初次解析和分页294–342ms，平均每页绘制48–53ms，进程峰值10.4–13.0MiB。
现有渲染基线：初次解析和分页161–292ms，平均每页绘制16–30ms，进程峰值4.6–5.4MiB。
这些值不含墨水屏刷新，不能直接推导正式应用的触摸响应。
重开与改字号后的书签恢复、坐标命中文字、两张图片资源加载均通过。
代码页和图文页均通过隔离预览程序显示；framebuffer截图与实机生成PGM逐像素一致。
两次预览会话恢复日志均为exit=0、restore_failed=0，结束回到系统主页。
触摸用内核事件注入，未验证实体触摸与物理残影；预览不代表已实现实时阅读UI。
具体指标和截图保存在上述output目录。

后续接入所需工作
RSS保留列表与文章切换，只替换正文渲染；加入受控HTML/CSS和离线图片资源解析。
现有UTF-8位置与CREngine DOM书签需做显式兼容，不可直接覆盖旧进度字段。
触摸需补按下/移动/抬起及长按，再接坐标选词、词典和划线存储。
需要真机内存、刷新、休眠/恢复及图文压力测试。
CREngine及其依赖的许可不等同于本项目MIT；发布任何组合产物前应逐项核对上游许可和分发要求。

来源
CREngine: https://github.com/koreader/crengine
HarfBuzz: https://github.com/harfbuzz/harfbuzz
libunibreak: https://github.com/adah1972/libunibreak
libpng: https://github.com/pnggroup/libpng
xxHash: https://github.com/Cyan4973/xxHash

应用内实验接入（2026-10-27）
默认 CMake 构建仍使用原排版。显式传入 RSS_CRE_SOURCE 才启用 CREngine；
build.py 使用已校验的固定源码和独立依赖前缀：
rtk proxy python3 native-rss/experiments/cre-reader/build.py host app
rtk proxy python3 native-rss/experiments/cre-reader/build.py arm app
主机全库验证需 RSS_FONT 指向 vendor/runtime/reader.ttf；ctest --test-dir output/cre-reader-20261027/in-app-host。
正常构建15项与CRE构建17项测试通过。新增验证涵盖受控HTML、缓存图像、失败回退、
原排版位置保留、独立DOM书签、字号重排、底栏翻页、收藏和下一篇。
app_launcher.py 生成正式 launch.sh 的独立实验副本：只更换 APP 并增加私有临时缓存生命周期。
参数只接受 /mnt/us/rss-backups/rss-cre-app.<字母数字>，防止路径注入；模板变更时拒绝生成。
STATE 必须保留 /var/local/native-rss，与原版共享会话锁，防止两个应用同时接管屏幕和数据库。
DB 保留现有 /mnt/us/native-rss/data/rss.sqlite；部署前必须正常退出原应用并用 SQLite .backup 留存快照。
不覆盖原二进制、启动器和书库入口。单独新增“RSS 新排版测试”Scriptlet，指向实验启动器。
启动器创建 /tmp/rss-cre-session.*（权限位0700，可继承setgid），导出 RSS_CRE_CACHE；恢复监护进程继承其路径，
正常退出或启动器被杀后均先关闭阅读器、清理自己的临时目录，再恢复系统和USB服务。
设备模式拒绝没有该缓存会话的新版渲染；宿主测试可使用自动清理的单篇临时目录。

使用：书库 → RSS 新排版测试 → 任意文章 → 正文中央 → 排版 → 新版排版。
选择会记住，后续文章直接用新版；同一面板可以切回“原排版”。
RSS 列表、收藏、下一篇和图片面板仍由原应用处理；不打开外部阅读器。
仅将清理过的结构标签、受控CSS和现有灰度缓存图像交给引擎；没有网络或任意本地资源访问入口。
512KiB正文、512页、200个图片节点及16MiB图像预算限制触发时提示并回退原排版。
reader_engine 保存选择；reader_cre:<文章ID> 保存“正文修订号:DOM书签”，不覆盖 entries.position。
正文修订后新版书签从首页开始；旧原排版进度保留。切换引擎时不保证落在同一段落。
如果历史 html 与当前全文 text 不一致，沿用已存全文，避免把正文退回摘要。
本次未实现选词、词典、批注和新版正文内图片点击放大；图片面板仍可查看。
组合二进制仅用于私人测试，尚未纳入正式安装包或发布流程；公开分发前需完成许可审计。

应用内真机验证（KPW3 / 5.16.2.1.1）
独立实验二进制静态ARM ELF，13,689,232字节，SHA-256：
8bbb12ac2cb0c8cf7ffe14de01d66f544b01f30b63503e7de29aa865f6fa36ab。
真实数据库已先做一致性备份与integrity_check；选用已读图文验证显示、底栏翻页、
返回列表后重开、两种排版往返切换。新版第2页重开/切换恢复后的截图与翻页时逐像素一致。
测试后数据库完整性为ok；全部文章正文、已读、收藏和原排版position与备份逐项对比均未变化。
真实图文会话VmHWM=22,760KiB，VmRSS=16,912KiB（单次观察，并非压力测试）。
正常退出日志exit=0、restore_failed=0，缓存会话目录移除，系统三项服务恢复。
另用独立虚构数据杀死实验启动器，确认PDEATHSIG终止阅读器且监护进程回收缓存和界面。
未验证实体手指操作、长时间连续阅读、极端订阅规模和物理残影；本次触摸由内核事件注入。
文章内首次转本地图片可能有等待；后续性能优化需另测，不能套用早期隔离样机耗时。
截图及会话结果保存在忽略的output/cre-reader-20261027/中，不纳入源码发布。

后续图片缓存修复：真实文章的8张2600×1740 RGBA PNG超过原STB32MiB预算。
加入同尺寸透明PNG回归素材，确认修改前失败；预算调整为64MiB，8MP、8MiB与解压限制保留。
原版15项、新版17项测试再次通过。设备8/8图片重试成功，修复工具VmHWM=42372KiB。
新版真机截图显示第2/9页图片；正文、已读、收藏与原排版进度对比备份无变化，完整性ok。
修复后二进制SHA-256为f4da15dc04160a70ad8757ca28ed1b33b58c912fd4293675345be99dcf311cb0。
仅更新独立测试入口的二进制，正式入口未覆盖；一次性修复工具已从设备移除。
