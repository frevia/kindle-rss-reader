# 验证记录说明

以下按开发时间记录，前面的“待验证”状态可能已由后续记录更新。原始设备报告与本地截图不随源码上传。

# KPW3 设备证据

2026-10-23 通过 USB 只读检查 `/Volumes/Kindle`。未安装或运行新应用。

`system/version.txt`：Kindle 5.16.2.1.1 (409747 002)。

设备既有兼容性报告（历史报告，不代表本项目实测）：

```text
selected_target=kindlepw2
architecture=armv7l
model=Paperwhite3
backend=classic
screen=1072x1448
bpp=8
stride=1088
buffers=4
rotation=3
driver_id=recognized
touch_protocol=0
page_keys=no
hardware_preflight=pass
firmware_contract=blocked
portable_probe=ready
```

硬件预检通过和固件契约阻塞同时存在，不能据此启用任意系统接管策略。
当前 framebuffer ioctl、输入设备和系统进程状态已通过下方只读诊断取得；系统接管行为仍需实测。

可见安装目录：`weread`、`extensions/weread`、`libkh/bin/fbink`、`koreader`。
KOReader 的 SSH 组件默认端口是 2222，设备已存在其 dropbear 程序。
RSS 应用不依赖 KOReader；如借用 SSH，只作为开发连接。

网络尝试：<device-ip> 的 2222/22 端口返回 Connection refused。

用户开启 SSH 后，<device-ip>:2222 曾成功以 root 公钥登录，实际输出：

```text
Linux kindle 3.0.35-lab126 #8 PREEMPT Tue Aug 1 12:49:59 UTC 2023 armv7l GNU/Linux
Kindle 5.16.2.1.1 (~~otaVersion~~)
```

后续连接再次超时；已改为通过 USB 传递诊断程序和报告。不要把一次成功登录当作连接持续可用。

不得以本报告替代启动/退出、休眠、USB、异常恢复和长期耗电的真机验收。
# 原生设备后端实现状态

`KindleDevice` 已实现 classic MXCFB 8 位屏幕映射、触摸独占读取及局部刷新。仅接受 1072×1448、rotation=3、`mxc_epdc_fb` 和 `cyttsp4_mt`；每 6 次有效更新做一次全刷，无像素变化不刷新。屏幕模式变化、事件丢失或 ioctl 失败会退出设备循环。刷新参数和触摸方向尚未实测，不能据编译成功判定兼容。

内核 ABI 核对来源：[Kindle MXCFB 接口](https://github.com/NiLuJe/FBInk/blob/master/eink/mxcfb-kindle.h)。使用 72 字节更新结构和 8 字节完成标记，GC16 波形、自动温度；代码仅声明所需 ABI，没有引入 FBInk 实现或 KOReader 运行依赖。触摸使用本仓库既有 KPW3 记录中的 ARM32 16 字节事件格式。

`device` 入口与桌面共用 `UiTasks`，提供后台同步、取消、OPML 导入导出。SIGTERM/INT/HUP 请求退出，释放输入和映射。不能通过手工设置环境变量视为满足生命周期条件。

## 会话启动器（尚未真机验收）

`device/launch.sh` 与 `device/kual/` 已提供开发入口。启动前检查电源、屏幕参数、工具及系统进程身份，使用文件锁防止重复运行，保存可见屏幕；暂停 awesome、状态栏、volumd 并关闭 pillow 浮层。退出时停止应用，检查进程启动时间后恢复原进程；仅在仍唤醒且原窗口管理器身份一致时恢复截图并刷新。最后恢复 volumd，避免在关闭数据库之前启动 USB 磁盘导出。

检测到 `/var/local/system/userpasswdenabled` 时拒绝启动，在任何系统服务操作前返回。

应用运行期间 USB 仅用于充电或已配置的网络，不会进入 USB 磁盘模式；退出后恢复系统 USB 处理。此策略参考 [KOReader 平台启动器中的系统服务处理](https://github.com/koreader/koreader/blob/master/platform/kindle/koreader.sh)，本应用不调用或依赖 KOReader。

本机模拟覆盖正常退出、应用退出码 7、休眠、电源查询失败、pillow 失败、部分进程暂停失败及 PID 复用。模拟不能证明真实 LIPC 属性、timeout 命令语法或系统服务行为正确。启动器启动独立恢复进程，在接管系统前保留屏幕快照、原始进程身份和会话锁。启动器消失或成为僵尸时，恢复进程执行恢复；设备程序在打开数据库和设备资源前绑定 Linux PR_SET_PDEATHSIG(SIGKILL)，并复核父进程，避免孤立程序继续写屏幕。应用继承专用 FIFO 的写描述符；恢复进程等到其关闭、确认应用真正退出后才恢复屏幕与 USB，不能仅以发出 SIGKILL 代替退出确认。生产启动器使用 shell 内置 kill，避免孤立的外部 STOP 命令晚于恢复执行。本机模拟已覆盖启动器 SIGKILL 后的恢复，但 Linux 内核父进程死亡行为及实际设备恢复仍待真机验证。恢复进程与启动器同时被强杀或设备掉电不在此机制的保障范围，不能据模拟测试发布正式安装包。

设备循环在真实触摸后调用 `lipc-set-prop -i com.lab126.powerd touchScreenSaverTimeout 1`，最多每 5 秒一次，不持续抑制休眠。命令通过 posix_spawn 的独立进程组执行，参数不经过 shell，2 秒超时后杀死并回收子进程；失败会退出设备循环交回启动器。命令成功、失败、超时、缺失程序、字面参数和回收均有本机测试，但 LIPC 的实机效果待验收。

## 只读诊断部署

2026-10-23 USB 再次挂载后，仅新增 `extensions/native-rss-diagnostics/` 与 `documents/RSS设备诊断.sh`。没有安装或运行完整阅读器。复制的 ARM 二进制回读 SHA-256 为 `ba591ebc811c5f4ffcde2e6fc5570de1c856b6a741cd48b08d1a224fbd5333f8`。诊断只调用 `probe`、读取工具和系统状态，将输出写入 `documents/RSS-device-report.txt`；用户已在设备运行，报告已读取并归档。不会写 framebuffer、独占触摸或暂停系统进程。


## 真机只读诊断结果（2026-10-23 03:06:23 UTC）

原始输出见 本地归档的 device-report-2026-10-23.txt（原始报告不随源码上传）。本项目 ARM 程序实际运行，`probe_exit=0`，确认：

- framebuffer：1072×1448、stride=1088、8 bpp、rotation=3、offset=0，驱动 `mxc_epdc_fb`；布局检查通过。
- 触摸：`/dev/input/event1`，`cyttsp4_mt`，X=0..1071、Y=0..1447。尚未验证实际触摸事件和坐标方向。
- `timeout` 秒数语法可用；flock、stat、pidof、LIPC、initctl 和已安装 FBInk 均存在。
- powerd 为 active；awesome、JunoStatusBarDriver、volumd 均存在且处于 S 状态；未检测到密码锁标记。

这证明二进制可以在该固件执行，且只读硬件参数满足启动器要求。尚未执行屏幕刷新、触摸独占、LIPC 写操作或系统进程暂停；显示、翻页、正常退出、休眠和异常恢复仍未验收。

## 内核生命周期自检准备

已将独立 ARM `process_test` 放入设备诊断目录，并提供 `documents/RSS恢复机制自检.sh`。程序通过自己的父子测试进程验证 PR_SET_PDEATHSIG，以及命令超时和子进程回收；不操作 framebuffer、输入设备或系统服务。二进制 SHA-256：`2619f3d0dc7885e81734cfca60ed18fd1230093d4acb1a2b1ecd1042ae8af4cf`。待用户运行后读取 `documents/RSS-process-report.txt`；部署不代表测试通过。

## 会话日志与诊断导出

启动器在私有目录 `/var/local/native-rss/` 保存 `session.log`，下次启动将其轮换为 `session.previous.log`。日志记录启动、阅读器进程号、恢复开始和结果，并保存程序错误输出；日志路径为符号链接时拒绝启动，避免覆盖其他文件。日志在取得会话锁后打开。

开发包提供 KUAL「导出 RSS 诊断报告」与 Scriptlets「RSS设备诊断.sh」，将只读硬件信息及两份日志各最后 64 行导出至 `documents/RSS-device-report.txt`，可在重新连接 USB 后读取。诊断不接管屏幕、不独占触摸、不暂停系统服务。该导出入口已打包，尚未部署完整阅读器。

### 首次进程自检结果

2026-10-23 03:29:58 UTC 从 KUAL 运行后生成报告，退出码 134，未捕获异常为“系统命令失败：No such file or directory”；原始报告归档为 `process-report-2026-10-23-v1.txt`。不能据此判定完整自检通过。测试末尾使用固定 `/bin/test` 路径，已改为 `/bin/sh` 内置比较并增加逐阶段输出。v2 自检已部署，待再次运行以确认具体结果。

### v2 自检结果无效：交叉编译断言被禁用

v2 报告返回 0，但没有阶段输出。核对发现 Zig ARM 工具链在 `-Os` 下默认定义 NDEBUG，先前手工编译的自检程序未覆盖这一默认值，不能证明断言检查通过；v1 也不能作为父进程死亡保护证据。v2 原始报告保存在 `process-report-2026-10-23-v2-invalid.txt`。

已显式增加 `-UNDEBUG`，并在测试源加入 NDEBUG 编译错误保护；`tools/build_process_test.py` 固定正确编译参数。v3 已重新编译部署，核对二进制包含断言条件字符串，等待重跑。此问题只影响手工编译的 ARM 自检；本机 CMake 测试目标原本均带有 `-UNDEBUG`。完整阅读器仍未部署。

### SSH 恢复与 v3 真机自检通过

2026-10-23 已通过 `root@<device-ip>:22` 公钥登录；旧端口 2222 超时。直接执行 `timeout 15 /mnt/us/extensions/native-rss-diagnostics/process_test`，输出如下：

```text
parent_death_protection=passed
spawn_shell=passed
timeout_and_reaping=passed
literal_arguments=passed
process_test_exit=0
```

此次执行的 v3 二进制显式开启断言，并核对过部署 SHA-256。内核父进程死亡保护、命令启动、超时/回收和字面参数测试通过。只验证这些进程行为，不能代替系统界面接管和异常恢复实测。

随后只读检查：powerd=active，awesome/JunoStatusBarDriver/volumd 均为 S 状态，mkfifo 可用，USB gadget connected=1。完整阅读器尚未安装。下一步界面接管测试前应断开 USB 磁盘连接，保留 Wi-Fi SSH。

## 首次原生界面真机启动（2026-10-23）

通过 SSH 安装开发包，37 个文件的设备端 SHA-256 校验通过。首次启动因 `/var/local` 继承 setgid 导致新目录权限为 2700，被只接受 700 的检查拒绝；尚未发生系统接管。修复为接受 root 所有且无组/其他访问权限的 700 或 2700，增加对应模拟测试。

重试后本项目原生空态界面成功写入并刷新 framebuffer，截图保存于 `output/device-first-launch.png`，中文排版和方向正常。20 秒后测试超时发送 TERM，启动器退出码 143，日志 `restore_failed=0`；随后确认阅读器进程不存在，awesome、状态栏、volumd 均恢复到 S 状态，截图 `output/device-after-exit.png` 显示系统主页已恢复。会话日志保存于 `output/device-first-session.txt`。

这证明首次显示及 TERM 退出恢复路径已实测；物理触摸、菜单正常退出、睡眠、强杀恢复、真实订阅同步和持续翻页尚待验证。USB 数据线仍连接但 gadget-lun0/file 为空，主机未挂载 Kindle 磁盘，测试时没有并发磁盘导出。

### 物理触摸、拼音选词与菜单正常退出

用户实机确认：点击搜索、输入 keji、选出“科技”、取消并经导航“退出应用”均正常，已返回系统主页。SSH 读取上一轮会话日志：`session_start Fri Oct 23 04:04:55 UTC 2026`，`session_cleanup recovering=0 exit=0`，`session_end exit=0 restore_failed=0`。这与用户反馈一致，支持物理触摸和菜单退出验收。

读取时设备已有新会话（04:06:50 UTC，reader pid 26626），系统服务处于 T 状态；这属于新会话接管，不能当成上一轮恢复失败。真实订阅、离线正文连续翻页、休眠及强杀恢复仍待验证。

### 真实订阅与设备端缓存正文渲染

通过设备 CLI 在独立 `data/verification.sqlite` 添加 `https://www.solidot.org/index.rss` 并同步，退出码 0；本地回读数据库 `integrity_check=ok`，1 个来源、20 篇文章、正文共 6651 字符。设备 CLI 使用该缓存库渲染最长文章（610 字符），退出码 0，产物 1072×1448，截图保存为 `output/device-cached-article.png`。此渲染路径只读缓存，不请求网络；尚未通过物理断网及连续翻页验证完整离线体验。测试未改动正式 `data/rss.sqlite` 订阅列表。

### 休眠唤醒验收

用户实机确认在阅读器中按电源休眠、等待后唤醒，系统触摸正常。SSH 核对会话（04:06:50 UTC 开始）已结束，日志 `session_cleanup recovering=0 exit=0`、`session_end exit=0 restore_failed=0`；powerd 为 active，awesome、状态栏、volumd 均为 S，阅读器进程已不存在。单次手动休眠恢复通过；长期耗电和多次循环仍未测量。

### 两种 SIGKILL 真机恢复

在确认设备唤醒、没有已运行阅读器且未导出 USB 磁盘后，分别新建测试会话，并核对阅读器 PPid 属于本次启动器：

- 应用 SIGKILL：launcher 28092 / reader 28162，启动器返回 137，日志 `recovering=0`、`restore_failed=0`，阅读器已消失。
- 启动器 SIGKILL：launcher 28265 / reader 28335，启动器返回 137，独立恢复进程输出 `recovering=1`、`session_end exit=0 restore_failed=0`，阅读器已消失。

日志分别归档 `output/device-reader-kill.txt`、`output/device-launcher-kill.txt`。恢复后 awesome 短暂 D 状态，随后复核三个系统服务均为 S；主页截图见 `output/device-after-launcher-kill.png`。这些实测覆盖运行中两种强杀恢复，不代表掉电、磁盘故障或恢复进程同时被杀的保障。

## 2026-10-23 图片与版式修复

用户反馈图片未显示。只读取得正式库副本，15 张图片均为静态 WebP，旧 JPEG/PNG 解码路径误报尺寸限制。新增 libwebp 1.6.0，保持 8 MiB / 2048 边长限制，动态 WebP 明确拒绝。修复前新增 WebP 用例复现失败，修复后通过；副本重新同步 15/15 图片缓存成功。

列表调整标题、来源日期的层级；阅读首页单独排版标题和首图预览，顶部提供完整图集入口。分页保留原有标题前缀字节位置兼容已有阅读进度；短首页分页连续性和直接图集入口已有回归测试。仍未实现逐段图文混排。

真机更新结果：ARM 静态 ELF 8,468,532 字节已安装并通过 SHA-256 校验；正式数据库在设备重新同步后 15/15 图片缓存成功，导出只读副本 integrity_check=ok。设备上渲染文章首图成功，已启动新版列表，session reader_started pid=3084。证据 output/device-refined-reader.png、device-refined-list.png。未覆盖逐段图文混排或长期连续阅读。

## 2026-10-23 原文图文分页

根据用户要求，撤销首图预览方案，按现有 HTML 中 img 的位置生成正文分页。短 figcaption 跟随图片，长图注继续分页，图片完整缩放且点击对应大图。重复 URL 在正文中保留每次出现的位置。现有 title+text 字节位置继续有效，连续图片页另存 occurrence 锚点，不迁移或替换文章数据库。

HTML 位置提取由原生 GPT-6 Luna High 实施；主线程独立复核后修正每字节映射的额外内存占用、包装节点内正文误判图注和嵌套图片图注范围重叠。分页与交互由主线程实现。测试覆盖原文顺序、字节内容完整性、图片不越界、短图注同页、连续图片退出重开、对应大图与字号变化。实际文章离线库生成 16 页，15 张图按原文排列；检查证据 output/inline-pages/contact.png。

真机验证：ARM ELF 8,486,892 字节，已经 SSH 安装，变化文件 SHA-256 全部匹配。设备通过 sqlite3 .backup 创建独立验证库，设置该副本阅读位置 626，原库未改动；ARM 程序生成第 2/16 页图片与图注，证据 output/device-inline-article.png。正式库 integrity_check=ok，图片 15/15 缓存。新版成功启动 pid=10854。

测试边界：本次解析、存储、分页、App 交互、设备接口、子进程、图片、拼音、打包和 ARM 检查通过，network 首次超时后复测通过；launcher 模拟测试多轮出现 10/15 秒超时及 started 标记等待失败，没有认定全部测试通过，未修改超时阈值或启动器实现。主机运行其它高负载任务，负载因素仍需后续隔离复核。

### 后续复测与验收清单校准

在不改启动器、测试代码或超时阈值的情况下，launcher 单独复测通过（23.22 秒）；随后完整 CTest 12/12 通过（27.94 秒，launcher 25.60 秒、network 1.20 秒）。这证明当前环境下失败不再复现，但不能单凭复测断言此前超时根因已定位。

已校准 PLAN.md 和 README.md：如实记录已完成的中文显示、触摸拼音、同步、原文图文渲染与单次恢复证据；保持四角触摸、物理断网连续翻页、USB 挂载、长时间耗电等待验收项。当前 SSH 连接超时，未重复安装仅文档变化；本地开发包包含更新后的记录。已请求用户执行关闭 Wi-Fi 后累计 50 次翻页与大图返回测试，等待实际结果。

### 设备端 OPML 往返验证

设备 SSH 恢复可访问后，使用现有 ARM 程序在 /var/local/native-rss 下 mktemp 独立测试目录，导入含重复 URL、中文名称、XML 转义与科技/编程嵌套分组的 OPML。重复导入两次仍只有 2 个来源；导出后导入第二个库，以 cmp 比对 URL、名称、分组完全一致，第二库 integrity_check=ok。测试文件清理成功。证据 output/device-opml-roundtrip.txt，exit=0。未接触正式订阅库或接管阅读界面；此项只证明设备 CLI 往返，不替代 GUI 导入按钮或 USB 操作验收。

### 用户离线翻页验收与图注对齐

用户对“关闭 Wi-Fi、累计前后翻页 50 次、点大图再返回，检查图片/残影/漏触/错页/卡顿”的请求明确回复“已测，显示和翻页正常”。据此勾选物理离线阅读与连续翻页，不推断尚未要求的字号调整或 USB 项目通过。随后按用户反馈将正文多行图注逐行居中，大图页面图片说明也统一居中。

### 阅读中 USB 插线与菜单退出验证

用户按“阅读器打开时连接 USB，再从菜单退出，确认电脑能看到 Kindle 磁盘”操作，回复“已退出，电脑能看到 Kindle 磁盘”。主机观察 /Volumes/Kindle 已挂载，以 SQLite mode=ro 只读访问正式库：integrity_check=ok、foreign_key_check 无错误、15/15 图片有缓存。安装二进制 SHA-256 与本地居中图注版本完全匹配。证据 output/device-usb-exit-check.json。未改动设备文件、未强制卸载；未单独制造同步写入瞬间插线压力场景，尚不能替代长期循环验收。

### GUI OPML 验收

用户确认界面导入导出均正常，订阅未重复。SSH 读回新生成的 subscriptions.opml，解析后的 outline 属性与导入前准备的 feeds.opml 完全一致；正式库活动来源仍为 1 个，integrity_check=ok。只读核对证据 output/device-gui-opml-check.json。GUI 入口据此验收通过。

### 阅读位置、字号与收藏持久化验收

用户按“打开文章翻到中间、调大字号并收藏、退出应用后重新打开”的步骤测试，明确回复“位置、字号和收藏都正常”。据此确认字号保留、恢复到原正文附近以及收藏视图可找到文章；不推断未执行的分组、已读标记或长期循环测试通过。

### 墨水屏操作图标（待实机部署）
新增原生矢量线条图标：RSS 标识、同步、搜索、收藏状态、翻页、阅读设置、图片入口及导航操作。图标与中文标签并列，保持原触摸区域和正文分页。统一列表分隔线与页脚边界。宿主机 render/app 测试通过，覆盖所有图标边界裁剪；列表和导航实际渲染预览已检查。SSH 超时，尚未部署或验收墨水屏显示效果。

图标版后续部署：SSH 恢复后确认阅读器未运行、USB 存储未导出，增量安装并通过 SHA-256 核对。启动日志 reader_started，读取真实 framebuffer 确认列表中的 RSS、同步、搜索和翻页图标正常；证据 output/device-icons.png。物理触摸与墨水屏残影尚未由用户验收。

### 书库入口封面与名称
根据用户实拍修正 Scriptlets 入口：Name=RSS 阅读器，Author=RSS Reader，Icon=/mnt/us/native-rss/book-cover.png，保留原启动命令并加 DontUseFBInk。专用黑白封面纳入安装包。设备校验文件哈希后，仅将启动脚本临时移出 documents、triggerUpdate 确认旧入口移除，再放回重新扫描；只读查询 cc.db 确认名称、作者和封面路径均已更新。RSS 数据库与其他书籍未改。诊断入口仍保留。

### UI 第一轮美化
列表改为标题在前、来源日期在后，64 px 页边距，导航左对齐并使用竖线/勾选表达选中态，阅读设置增加控件边框，页脚增加阅读进度细线。app/render/package/arm_artifact 检查通过。设备正常退出 restore_failed=0 后增量部署，全部文件哈希核对成功；新会话 reader_started pid=9246，设备 framebuffer 证据 output/device-ui-v2.png。尚未代替用户的物理触摸和残影验收。
