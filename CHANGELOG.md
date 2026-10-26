# RSS Reader v0.0.2

面向 Kindle Paperwhite 3（KPW3）的开发验证版，已验证设备固件为 5.16.2.1.1。安装包由 GitHub Actions 从对应标签源码重新编译，包含 MIT 与第三方许可。

## 新增

- 多级订阅分组：新增空分组、创建子分组、重命名、整棵子树移动及嵌套 OPML 导入导出。
- 订阅名称、RSS 地址和分组编辑；名称留空时同步自动发现博客名称。
- 后台同步进度、取消与失败订阅重试；离开同步页仍继续执行。
- AVIF 离线图片缓存，大尺寸 WebP 解码时缩放，保留像素与内存限制。
- 越狱后安装教程、贡献指南、安全反馈、MIT 许可和第三方组件说明。

## 改进与修复

- 重整首页、订阅管理、分组筛选、阅读工具栏和拼音输入界面。
- 首页首次显示准确的当前页与总页数；复用标题测量结果，优化翻页、图标和屏幕差异检测。
- 源码包按文件白名单收集，排除数据库、OPML、日志与原始诊断 TXT；两种 ZIP 都携带完整许可。
- GitHub Actions 自动运行宿主测试、校验固定工具链与依赖、编译 ARM、验证安装文件并生成附件 SHA-256。

## 安装与升级

下载 `RSS-Reader-v0.0.2-KPW3.zip`，解压后将 `native-rss`、`extensions`、`documents` 三个目录的内容合并到 Kindle 用户盘根目录。先准备已越狱的 KPW3、KUAL、固定路径的 FBInk 与必需工具；Scriptlets 仅用于可选书库入口。

升级前退出应用并备份整个 `native-rss`，保留 `native-rss/data/`。安装包不包含个人数据库；不要整体替换书籍或扩展目录，也不要使用“更新 Kindle”安装 ZIP。

[完整安装、升级与排错教程](https://github.com/frevia/kindle-rss-reader/blob/v0.0.2/native-rss/docs/INSTALL.md)。

## 验证与限制

发布流程会先执行宿主构建和 CTest，再重新编译 ARM 静态程序，核对 ELF、源码输入、二进制与安装文件哈希。构建或校验失败时不发布 Release。

KPW3 已有图文阅读、OPML、阅读状态与基本生命周期验证记录。本次 GitHub 产物未部署真机，不将编译通过视为实机验收；长期耗电、重复休眠 / 退出 / USB 恢复及其他机型仍需验证。未实现通用网页全文提取，不支持动态图片。

与上一版的变更：[v0.0.1...v0.0.2](https://github.com/frevia/kindle-rss-reader/compare/v0.0.1...v0.0.2)。
