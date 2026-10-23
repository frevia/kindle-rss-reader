# RSS Reader

面向 Kindle Paperwhite 3 的独立原生 RSS 阅读器，C++17 实现。

支持 RSS/Atom、离线图文阅读、拼音搜索、订阅分组、收藏与阅读位置保存、OPML 导入导出。

## 下载与安装

前往 [Releases](https://github.com/frevia/kindle-rss-reader/releases) 下载 **KPW3 安装包**。当前 `v0.0.1` 附件名为 `RSS-Reader-v0.0.1-KPW3.zip`；不要下载源码包作为安装包，也不要使用“更新 Kindle”安装。

需要已具备自制程序运行环境的 KPW3、KUAL 及必需工具；书库入口另需 Scriptlets。将包内三个目录合并到 Kindle 用户盘，安全弹出后从 KUAL 启动。

**[完整安装教程：运行条件、目录结构、订阅导入、保留数据升级与故障排查](native-rss/docs/INSTALL.md)**

`main` 可能包含尚未发布的改动，安装包功能以对应 Release 为准。

## 开发文档

源码位于 [`native-rss/`](native-rss/)，构建与使用说明见 [项目文档](native-rss/README.md)，界面设计见 [UI.md](native-rss/docs/UI.md)。

当前为开发验证版。长期耗电和反复生命周期仍需实机验证。

本仓库不包含私人订阅数据库、实机原始诊断报告、编译产物或运行日志。第三方代码许可保留在 `native-rss/third_party/`。
