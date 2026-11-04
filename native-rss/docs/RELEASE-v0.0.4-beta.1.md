# RSS Reader v0.0.4-beta.1

已发布的 Kindle 兼容与在线升级测试版。[打开 beta.1 下载页面](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.4-beta.1)。已有真机开发记录来自 KPW3，其他机型仍待验证；发布 beta 不代表完成机型验收。

## 包含的功能

包含 v0.0.3 之后的自动同步、文章与选区翻译、多服务商配置、在线升级和 Kindle 兼容代码，完整变化见[更新记录](../../CHANGELOG.md)。其他机型按[兼容测试流程](COMPATIBILITY.md#测试安装流程)先诊断再试用，并使用[反馈模板](COMPATIBILITY.md#怎么反馈兼容情况)提交结果；首页适配范围仍仅限 KPW3／5.16.2.1.1。

## 测试方法

验证在线升级时，先备份，再将 beta.1 安装包的三个目录合并复制到 Kindle（保留 `native-rss/data`），启动后进入「菜单 → 设置 → 在线升级」，应发现 beta.2。完整步骤见[在线升级测试](INSTALL.md#在线升级测试)。只测试普通阅读与机型兼容时，可直接安装 [beta.2](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.4-beta.2)。

- beta.1 和 beta.2 功能相同，仅版本号不同，用于验证检查、下载、校验、安装和重启流程。
- beta 版会检查本仓库最近 100 条发布中的后续 beta 和正式版，按版本号选择；正式版继续只检查正式更新。
- 本次升级测试先通过 USB 安装 beta.1；如果直接安装 beta.2，需等待更高版本才能测试在线升级。
- 升级前停用首页 RSS，连接 Wi-Fi，至少保留 400 MB 空间；安装过程中不要接 USB 或关机。
- 升级应保留订阅、翻译配置、阅读记录与缓存。测试前自行备份；真实断电恢复尚未验证。

## 下载文件

- `RSS-Reader-v0.0.4-beta.1-Kindle.zip`：安装包。
- `RSS-Reader-v0.0.4-beta.1-source.zip`：对应项目源码。
- `RSS-Reader-v0.0.4-beta.1-third-party-sources.zip`：第三方对应源码。
- `SHA256SUMS.txt`：SHA-256 校验文件。

许可与第三方声明见 [COPYING](../../COPYING) 和 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)。
