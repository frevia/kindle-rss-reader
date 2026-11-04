# Kindle 机型兼容

当前源码的兼容版使用同一份静态 ARMv7 程序，按设备实际能力检查屏幕、输入和系统会话。已有设备验证记录来自 KPW3／5.16.2.1.1；本轮改变了刷新路径，仍需重新实机验收。其他机型的纯函数、事件回放和会话模拟通过不等于真机支持认证。

## 当前条件

| 项目 | 条件与行为 |
| --- | --- |
| 程序 | ARM32／ARMv7、静态 EABI5 soft-float；不加载设备的 glibc，不按系统动态链接器复制两套应用。ARMv5／ARMv6 老设备不在范围内。 |
| 屏幕 | 8 位灰度、竖屏，宽 600–2560、高 800–3400；校验 stride、偏移、虚拟高度和实际映射内存。其他位深拒绝启动，不修改系统屏幕模式。 |
| 界面 | 保留 1072×1448 逻辑排版，等比缩放并居中留白；输入反向映射至同一画面。KPW3 尺寸不缩放，其他设备的分页与逻辑字号保持一致。 |
| 刷新 | 使用设备已有的 `/mnt/us/libkh/bin/fbink`，启动前检查机型识别、尺寸与 stride 一致性，拒绝 sunxi 和反色 framebuffer；按变化区域刷新，每六次有效更新全刷。刷新检查退出码与有界诊断输出，等待错误或未知诊断会退出设备循环，不提交成功帧。FBInk 必须支持实际硬件，不能以文件存在代替识别成功。 |
| 触摸 | 根据 evdev 能力发现触摸屏，支持 MT Type B、MT Type A、已知 Basic 2019 面板的 Snow 协议和单点 ABS／BTN_TOUCH；Type B 以首个活动槽为主触点，逐槽保留坐标，支持非零槽、多指取消、长按、移动及事件丢失保护。多个不明确候选会拒绝启动。 |
| 系统会话 | 需要 Upstart、可解析的 Kindle 5 固件版本、可验证身份的 awesome／volumd 和 LIPC 电源／浮层接口；Juno 状态栏存在时一并接管。发现 `statusbar.conf` 时查询独立状态栏，将运行进程纳入同一暂停／恢复流程；未知状态拒绝启动。未满足条件时保持拒绝，不绕过 USB 数据保护。 |
| 电量 | 优先通过 LIPC 读取 powerd 的 `battLevel`／`isCharging`，查询失败或数据无效时逐项回退 sysfs；没有有效电量时显示未知。 |

尺寸回归覆盖 600×800、758×1024、1072×1448、1236×1648、1264×1680、1404×1872 和 1860×2480。分辨率相同不代表驱动、触摸方向、电源与固件相同，不能据此生成已支持机型白名单。

本轮不启用运行中的自动旋转、Voyage 压感翻页键、Oasis 特殊按键或 Scribe 手写笔。请在系统正常竖屏姿态启动；屏幕布局运行中改变会停止设备绘制，恢复时不把旧快照写入新布局。触摸轴向仍需各机型实际校准。

## 面板与固件差异

Basic 2019 的部分面板使用带 tracking ID、没有标准 MT slot 的 Snow 协议。识别到该机型后才启用例外，保留坐标增量和重复 ID，不把每个报告都当成新按下；全局抬手清空接触，多指持续取消至全部释放。同机型存在标准面板修订，有 MT slot 时优先 Type B，Snow 收到标准释放事件时回退。识别仅在本机有界读取设备代码，不写入完整序列号；未知机型不会自动套用该例外。

新固件的独立状态栏可能绕过 Pillow 与 awesome 继续绘图。启动器从 Upstart 查询其 PID，并核对进程名和启动时间，暂停该进程的全部线程；退出和启动器强杀恢复均遵循相同身份检查，不向已替换进程发送信号。原本未运行的状态栏不会被启动；阅读期间发现状态栏启动、重启或身份变化，会退出阅读并恢复系统。此处不停止／重启系统任务，也不修改固件或服务配置。

启动器按 `/etc/prettyversion.txt` 选择成对的 Pillow 操作：Kindle 5.6.5 之前使用 `interrogatePillow` 的 `hideMe()`／`showMe()`，5.6.5 起使用 `disableEnablePillow disable`／`enable`；正常退出、启动失败和强杀监护恢复使用同一策略。缺少版本、格式异常、重复版本或非 Kindle 5／SysV 系统在界面接管前拒绝启动。SysV 老设备的无触摸界面、旧 CPU 和显示模式不在现有 ARMv7 触摸版范围，不能仅跳过 Pillow 检查就宣布支持。

外部 LIPC／FBInk 命令由独立进程监护：监护拥有命令进程组、截止时间和阅读器生命周期管道。阅读器被强杀或消失时，监护终止命令及其同组后代；命令正常结束时也清理剩余后代。会话 FIFO 仍由真正运行的工具持有，系统恢复继续等待其实际关闭，避免孤立工具无限占用 FIFO，或在工具尚未退出时恢复界面与 USB。fork 后的监护仅使用预先准备的数据和异步信号安全的系统调用，不访问阅读器线程里的数据库、分配器或锁。

官方 FBInk CLI 的 `--refresh -w` 分支可能在等待失败时仍退出 0，所以刷新适配器同时检查最多 4096 字节的标准输出／错误输出。在 `-q` 模式下，仅允许没有诊断，或单条已知的“不支持可选提交等待”警告；后者不影响随后的完成等待。其他输出（包括完成等待失败、额外或未知警告）、输出超限、非零退出及超时都按失败处理。此策略依赖官方 FBInk 的错误诊断契约；定制版本或诊断格式变化会保守退出，不能静默忽略。对应上游实现见 [CLI 刷新等待](https://github.com/NiLuJe/FBInk/blob/master/fbink_cmd.c#L2534-L2545) 与 [WARN 诊断](https://github.com/NiLuJe/FBInk/blob/master/fbink_internal.h#L434-L445)。

## 机型配置与电量回退

`kindle_profile()` 保存 Touch、Paperwhite 1–6、Basic 2014–2024、Voyage、Oasis 1–3、Scribe 与 Colorsoft 系列的已知设备代码、DPI、显示平台提示及 Snow 例外。标准 evdev 能力优先于机型协议例外。DPI 仅用于诊断，当前仍采用统一逻辑画布；`classic`／`zelda`／`rex`／`mtk` 是平台提示，不是本项目已经验证的后端列表。

诊断新增 `model`、`display_dpi`、`display_platform_hint`、`touch_policy`、`historical_firmware`。只有 Paperwhite 3 记录历史固件 5.16.2.1.1；历史证据不代表本轮程序的真机验收，所以本轮明确输出 `current_build_hardware_verified=0`。未知或无效设备代码返回 `Unknown`、DPI 0，不启用 Snow；仍执行完整能力检查。完整序列号不会出现在报告中。

电量每 30 秒随原有状态采样一次。LIPC 查询使用固定程序与参数，不经过 shell；容量只接受 0–100，充电只接受 0／1。每项最多 200 ms、32 字节，两项分别回退到 sysfs，不因一项成功丢失另一项的回退值。诊断同时列出原始 powerd 查询与电源节点，便于区分服务不可用和节点差异。

## 刷新性能测量

退出设备会话时，日志新增一行 `display_metrics`：`frames` 是提交次数，`skipped` 是无像素变化的次数，`refreshes` 是成功刷新次数，`scale_us`／`diff_us`／`copy_us`／`fbink_us` 分别累计缩放、差分、复制、外部 FBInk 调用及等待的微秒数。只汇总数字，不记录正文。诊断报告末尾的会话日志可直接保留这行；被 SIGKILL 的阅读器无法输出自身汇总。

同一机型上分别测试列表、正文翻页、键盘和长按选词，记录会话时长、操作次数、上述耗时以及残影。用 `scale_us / frames` 衡量缩放成本，用 `fbink_us / refreshes` 衡量外部刷新成本（分母为零时不计算）；后者包含内核刷新等待，不能全部归因于 CLI 启动。原生 DPI 排版会改变分页、键盘和触摸布局，常驻 FBInk 会引入进程及恢复生命周期；在取得实机性能数据前保留现有路径，不以主机跑分替代这些判断。

## 第一次检查新设备

1. 保留个人数据备份，确认使用新兼容包（`-Kindle.zip` 或本地 `kindle-rss-kindle-dev.zip`）。旧 `-KPW3.zip` 不具备这些适配。
2. 从书库打开“RSS设备诊断”，或 KUAL → Kindle RSS → 导出 RSS 诊断报告。
3. 查看 `documents/RSS-device-report.txt` 的 `layout`、`input`、尺寸、stride、偏移、触摸协议和 FBInk 识别结果。屏幕不受支持时，报告仍继续列出输入能力。
4. 预检通过后再尝试阅读、输入、退出；分开记录触摸方向、长按、休眠唤醒、强杀恢复和 USB 退出表现。检查报告通过不替代这些验证。

报告不包含设备完整序列号。反馈时提供机型、固件、报告、故障时间和操作步骤，不上传数据库、登录配置或正文缓存。

## 首页卡片与在线升级

原生首页 RSS 卡片是独立的固件功能，仍只接受已核对哈希的 KPW3／5.16.2.1.1。普通阅读器的兼容范围不会放宽首页文件校验，见 [HOME.md](HOME.md)。

新安装包使用 `UPDATE.json` 的 `model: Kindle` 和 `-Kindle.zip` 附件名。兼容版拒绝旧 KPW3 包，防止其他设备在线安装回不兼容的程序；历史专用开发版先通过 USB 升级一次。个人数据库结构不因本次适配改变。

## 实现参考

参考本地微信读书安装包的职责划分，未复制其封装的程序或脚本。刷新参数遵循 [FBInk CLI](https://github.com/NiLuJe/FBInk/blob/master/CLI.md)，输入协议遵循 [Linux 多点触摸协议](https://docs.kernel.org/input/multi-touch-protocol.html)，电源节点对照 [KOReader Kindle 设备实现](https://github.com/koreader/koreader/blob/3d40440e7f07a156dfee6cffc50400654d2336da/frontend/device/kindle/device.lua)。各项目的硬件支持范围不等于本项目的实机验收范围。

电源属性与固件分界对照同一固定版本的 [KOReader powerd](https://github.com/koreader/koreader/blob/3d40440e7f07a156dfee6cffc50400654d2336da/frontend/device/kindle/powerd.lua) 和 [Kindle 启动脚本](https://github.com/koreader/koreader/blob/3d40440e7f07a156dfee6cffc50400654d2336da/platform/kindle/koreader.sh)。
