# RSS Reader

在 Kindle 上订阅博客和新闻，连网下载后，就能离线阅读文章和图片。

已有真机验证设备为 **Kindle Paperwhite 3（KPW3，2015 款）**。当前源码新增按屏幕和触摸能力运行的 Kindle 兼容版，其他机型仍需实机验收，具体条件见[机型兼容说明](native-rss/docs/COMPATIBILITY.md)。设备需要已经越狱，并完成越狱教程中的后续环境设置。

**兼容测试版 v0.0.4-beta.4** 包含订阅源发现、自动同步、翻译、在线升级及 Kindle 兼容代码，并修复安装后因会话锁未释放而回滚的问题。[下载 beta.4](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.4-beta.4) · [beta.4 发布说明](native-rss/docs/RELEASE-v0.0.4-beta.4.md) · [更新记录](CHANGELOG.md)。beta 发布不代表其他机型已经通过真机验收。

**beta.1／beta.2／beta.3 用户建议通过 USB 升级到 beta.4。** 旧启动器本身有锁交接问题，直接在线升级仍可能回滚；要测试在线升级，需先[修复旧启动器并重新打开阅读器](native-rss/docs/INSTALL.md#旧版升级前修复启动器)。只发布新安装包无法修复正在运行的旧启动器，历史 beta 附件保持不变。

当前正式版仍为 **v0.0.3**，[正式版下载入口](https://github.com/frevia/kindle-rss-reader/releases/latest)不包含 beta。本文和使用指南以 beta.4 为准；beta.1／beta.2 的旧订阅导航见[对应版本指南](https://github.com/frevia/kindle-rss-reader/blob/v0.0.4-beta.2/native-rss/docs/USAGE.md)，正式版差异见 [v0.0.3 发布说明](native-rss/docs/RELEASE-v0.0.3.md)；仍使用 v0.0.2 时，请参阅[该版本使用指南](https://github.com/frevia/kindle-rss-reader/blob/v0.0.2/native-rss/docs/USAGE.md)。

## 可以用它做什么

- 添加自己喜欢的博客或新闻订阅，把新文章下载到 Kindle。
- 断开网络后阅读已经下载的文章和图片。
- 尝试下载网页正文，支持单篇下载、按订阅自动下载新文章，并保留 RSS 原文。
- 使用保留标题、引用和代码层级的新版排版，也可切回原排版。
- 给订阅分组、搜索文章、收藏喜欢的内容，并保存阅读位置。
- 从其他阅读器批量导入订阅列表。
- 正文左右滑动翻页，同步进度可收起到后台。
- 可选[原生首页 RSS 书卡](native-rss/docs/HOME.md)，查看订阅摘要、未读数并直接打开文章；需要另行安装匹配固件的首页适配。

v0.0.4 beta 已提供以下功能，**正式版 v0.0.3 尚不包含**：

- [发现订阅源](native-rss/docs/USAGE.md#发现订阅源)（beta.3）：从 OPML 源库查找来源，按主题筛选后添加订阅，缓存后可离线查找。
- [自动同步](native-rss/docs/USAGE.md#自动同步)：默认开启，在打开阅读器和使用期间按 30 分钟间隔检查订阅，不唤醒休眠设备。
- [文章翻译与长按选词翻译](native-rss/docs/USAGE.md#文章翻译)：英文转简体中文，支持原文、译文、双语及离线缓存；可配置 Qwen、SiliconFlow、智谱或自定义 OpenAI 兼容服务，需自行提供 API Key，内容会发往所选服务并可能计费。
- [GitHub 在线升级](native-rss/docs/INSTALL.md#在线升级)：手动检查当前通道更新、下载校验并安装重启；首次启用需 USB 安装 beta，beta 通道包含后续 beta 和正式版。
- [Kindle 兼容版](native-rss/docs/COMPATIBILITY.md)：按屏幕、触摸与系统能力检查其他 ARMv7 Kindle；其他机型仍待真机验收，首页适配范围不变。

## 开始使用

1. 打开 [beta.4 下载页面](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.4-beta.4)。旧 beta 默认通过 USB 升级；已经修复启动器的 beta.3 可按[升级测试步骤](native-rss/docs/INSTALL.md#在线升级测试)在线升级到 beta.4。
2. 在 **Assets（下载文件）** 中下载 **`RSS-Reader-v0.0.4-beta.4-Kindle.zip`**。历史 **`-KPW3.zip`** 只供 KPW3 使用，不能用于其他机型。
3. 解压后，按[安装教程](native-rss/docs/INSTALL.md)把三个文件夹合并复制到 Kindle，再从书库或 KUAL 打开阅读器。
4. 连上 Wi-Fi，按[使用指南](native-rss/docs/USAGE.md#添加第一个订阅)添加第一个订阅，下载文章后就能离线阅读。

安装教程也包含升级、备份和卸载步骤。入口没出现或应用打不开时，再查看[启动问题排查](native-rss/docs/TROUBLESHOOTING.md)。

其他机型请按[测试安装流程](native-rss/docs/COMPATIBILITY.md#测试安装流程)先备份、确认兼容包并导出只读诊断，再逐项测试。无论成功或失败，都欢迎使用[兼容反馈模板](native-rss/docs/COMPATIBILITY.md#怎么反馈兼容情况)，通过 GitHub Issues 或邮件反馈；其他机型不启用首页 RSS 适配。

目前仍是开发中的版本，长期使用和反复休眠、退出还需要更多实际使用验证。

## 遇到问题

先查看[安装教程中的常见问题](native-rss/docs/INSTALL.md#打不开或用不了怎么办)。仍未解决时，可以[提交问题](https://github.com/frevia/kindle-rss-reader/issues)，描述你在哪一步遇到了什么情况。不要公开上传自己的完整阅读数据。

发现可能泄露数据的安全问题，请按[安全反馈说明](SECURITY.md)联系维护者。

## 参与项目

[更新记录](CHANGELOG.md) · [贡献指南](CONTRIBUTING.md) · [开发参考](native-rss/docs/DEVELOPMENT.md) · [开源检查清单](native-rss/docs/OPEN_SOURCE_CHECKLIST.md)

项目原创代码和文档采用 [MIT 许可证](LICENSE)，允许在保留许可说明的前提下使用、修改和分发。链接 CREngine-ng 的组合程序按 [GPLv3](COPYING) 分发；Release 同时提供项目源码、第三方对应源码及校验文件。用到的引擎、字体、词库等组件遵守各自许可，见[第三方组件说明](THIRD_PARTY_NOTICES.md)。本项目由社区独立开发，与 Amazon 无隶属关系。
