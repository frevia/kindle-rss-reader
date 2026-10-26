# RSS Reader

面向 Kindle Paperwhite 3 的独立原生 RSS 阅读器，C++17 实现。

支持 RSS/Atom、离线图文阅读、拼音搜索、订阅分组、收藏与阅读位置保存、OPML 导入导出。

## 下载与安装

前往 [v0.0.2 Release](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.2) 下载 **KPW3 安装包** `RSS-Reader-v0.0.2-KPW3.zip`；不要下载源码包作为安装包，也不要使用“更新 Kindle”安装。

需要已具备自制程序运行环境的 KPW3、KUAL 及必需工具；书库入口另需 Scriptlets。将包内三个目录合并到 Kindle 用户盘，安全弹出后从 KUAL 启动。

**[完整安装教程：运行条件、目录结构、订阅导入、保留数据升级与故障排查](native-rss/docs/INSTALL.md)**

`main` 可能包含尚未发布的改动，安装包功能以对应 Release 为准。

## 开发文档

源码位于 [`native-rss/`](native-rss/)，构建与使用说明见 [项目文档](native-rss/README.md)，界面设计见 [UI.md](native-rss/docs/UI.md)。

当前为开发验证版，仅验证 KPW3 / 固件 5.16.2.1.1。长期耗电、反复生命周期及其他机型仍需实机验证。

本仓库不包含私人订阅数据库、实机原始诊断报告、编译产物或运行日志。第三方代码许可保留在 `native-rss/third_party/`。

## 参与与许可

问题反馈与开发约定见 [贡献指南](CONTRIBUTING.md)，安全问题见 [安全反馈](SECURITY.md)。开源前的检查结果与待办见 [开源检查清单](native-rss/docs/OPEN_SOURCE_CHECKLIST.md)。

版本变更见 [CHANGELOG.md](CHANGELOG.md)。推送版本标签后，[GitHub Actions](.github/workflows/release.yml) 运行测试、重新编译 ARM 并发布带校验值的附件。

项目原创代码与文档采用 [MIT License](LICENSE)。第三方组件、词库、字体和证书保留各自许可，见 [第三方组件说明](THIRD_PARTY_NOTICES.md)。本项目为独立社区项目，与 Amazon 无隶属关系。
