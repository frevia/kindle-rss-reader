# RSS Reader

在 Kindle 上订阅博客和新闻，连网下载后，就能离线阅读文章和图片。

目前支持 **Kindle Paperwhite 3**，也常叫 **KPW3（2015 款）**。其他机型暂未验证。设备需要已经越狱，并完成越狱教程中的后续环境设置。

当前源码版本为 **v0.0.3**。以下文档对应此版本；变化与验证范围见 [v0.0.3 发布说明](native-rss/docs/RELEASE-v0.0.3.md)。仍使用 v0.0.2 时，请参阅[旧版本的使用指南](https://github.com/frevia/kindle-rss-reader/blob/v0.0.2/native-rss/docs/USAGE.md)。

## 可以用它做什么

- 添加自己喜欢的博客或新闻订阅，把新文章下载到 Kindle。
- 断开网络后阅读已经下载的文章和图片。
- v0.0.3可尝试下载网页正文，支持单篇下载、按订阅自动下载新文章，并保留 RSS 原文。
- v0.0.3支持保留标题、引用和代码层级的新版排版，并可切回原排版。
- 给订阅分组、搜索文章、收藏喜欢的内容，并保存阅读位置。
- 从其他阅读器批量导入订阅列表。
- v0.0.3支持正文左右滑动翻页、同步弹窗与后台运行。
- 可选[原生首页 RSS 书卡](native-rss/docs/HOME.md)，查看订阅摘要、未读数并直接打开文章；需要另行安装匹配固件的首页适配。

## 开始使用

1. 打开[最新版下载页面](https://github.com/frevia/kindle-rss-reader/releases/latest)。
2. 在页面下方的 **Assets（下载文件）** 中，下载文件名以 **`-KPW3.zip`** 结尾的压缩包。
3. 解压后，按[安装教程](native-rss/docs/INSTALL.md)把三个文件夹合并复制到 Kindle，再从书库或 KUAL 打开阅读器。
4. 连上 Wi-Fi，按[使用指南](native-rss/docs/USAGE.md#添加第一个订阅)添加第一个订阅，下载文章后就能离线阅读。

安装教程也包含升级、备份和卸载步骤。入口没出现或应用打不开时，再查看[启动问题排查](native-rss/docs/TROUBLESHOOTING.md)。

目前仍是开发中的版本，长期使用和反复休眠、退出还需要更多实际使用验证。

## 遇到问题

先查看[安装教程中的常见问题](native-rss/docs/INSTALL.md#打不开或用不了怎么办)。仍未解决时，可以[提交问题](https://github.com/frevia/kindle-rss-reader/issues)，描述你在哪一步遇到了什么情况。不要公开上传自己的完整阅读数据。

发现可能泄露数据的安全问题，请按[安全反馈说明](SECURITY.md)联系维护者。

## 参与项目

[更新记录](CHANGELOG.md) · [贡献指南](CONTRIBUTING.md) · [开发参考](native-rss/docs/DEVELOPMENT.md) · [开源检查清单](native-rss/docs/OPEN_SOURCE_CHECKLIST.md)

项目原创代码和文档采用 [MIT 许可证](LICENSE)，允许在保留许可说明的前提下使用、修改和分发。链接 CREngine-ng 的组合程序按 [GPLv3](COPYING) 分发；Release 同时提供项目源码、第三方对应源码及校验文件。用到的引擎、字体、词库等组件遵守各自许可，见[第三方组件说明](THIRD_PARTY_NOTICES.md)。本项目由社区独立开发，与 Amazon 无隶属关系。
