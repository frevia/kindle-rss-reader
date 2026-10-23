# 安装、升级与使用

本文对应 [RSS Reader v0.0.1](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.1) 的目录结构。面向 Kindle Paperwhite 3（KPW3），已验证固件为 5.16.2.1.1。

`main` 分支可能包含尚未发布的修改。2026-10-23 推送的列表和导航性能优化尚未包含在 `v0.0.1` 附件中；下载 Release 时以该版本的说明为准。

## 1. 确认运行条件

这是原生自制应用，需要设备已经具备运行自制程序的环境：

- 已越狱的 KPW3，以及可用的 **KUAL**；书库启动另需已经配置 **Scriptlets**。
- 设备上已有 `/mnt/us/libkh/bin/fbink`，以及可用的 `timeout`、`flock`、`stat`、`pidof`、`awk`、`dd`、`mktemp`、`mkfifo` 和系统 LIPC 工具。诊断入口可辅助检查环境，不能自动补齐依赖。
- 启动器检测到设备启用了密码锁时会拒绝启动。如果需要保持密码锁，请暂不运行本应用。

本包不提供越狱、KUAL、Scriptlets 或上述系统工具的安装，也不依赖 KOReader。尚未配置这些环境的原厂 Kindle，不能仅复制本 ZIP 就运行。其他 Kindle 型号和固件尚未验证。

**不要将 ZIP 放进“更新 Kindle”的固件更新流程。** 当前为开发版本，长期耗电和反复休眠、退出等场景仍需更多验证。

## 2. 下载正确附件

打开 [Release 页面](https://github.com/frevia/kindle-rss-reader/releases/tag/v0.0.1)，在 Assets 中选择：

| 附件 | 用途 |
|---|---|
| `RSS-Reader-v0.0.1-KPW3.zip` | 安装到 Kindle，普通用户下载这个 |
| `RSS-Reader-v0.0.1-source.zip` | 开发源码，不能直接安装 |
| `SHA256SUMS.txt` | 检查下载文件的 SHA-256 |
| GitHub 自动生成的 Source code | 源码归档，不能直接安装 |

可选校验：在电脑上计算安装 ZIP 的 SHA-256，与 `SHA256SUMS.txt` 中同名文件的一行比较。macOS 使用 `shasum -a 256 文件名`，Linux 使用 `sha256sum 文件名`，Windows PowerShell 使用 `Get-FileHash 文件名 -Algorithm SHA256`。

如果页面显示 404，先确认账号有该仓库的访问权限。

## 3. 复制到 Kindle 用户盘

1. 如果已装过 RSS Reader，先在应用内退出，连接 USB 后把整个 `native-rss` 文件夹备份到电脑。
2. 在电脑上解压安装 ZIP，再通过 USB 打开 Kindle 用户盘。这里的“根目录”是能直接看到 `documents` 文件夹的位置，设备内部对应 `/mnt/us`。
3. 将解压得到的 `native-rss`、`extensions`、`documents` 三个文件夹的内容，**合并**到 Kindle 用户盘的同名目录。
4. 安全弹出 Kindle 用户盘，断开 USB 磁盘连接，再启动应用。

复制后的关键路径应为：

```text
Kindle 用户盘/
├── native-rss/
│   ├── bin/kindle-rss
│   ├── launch.sh
│   ├── book-cover.png
│   ├── fonts/reader.ttf
│   ├── certs/ca-bundle.pem
│   └── INSTALL.txt
├── extensions/
│   └── native-rss/
│       ├── config.xml
│       └── menu.json
└── documents/
    ├── RSS阅读器.sh
    └── RSS设备诊断.sh
```

不要多套一层 `RSS-Reader-v0.0.1-KPW3/`，也不要把整个安装包放进 `documents`。**不要用空目录替换整个 `documents` 或 `extensions`**，其中可能有书籍或其他扩展。

如果电脑提示只能“替换文件夹”而不能合并，请进入各文件夹，只复制包内的文件和子目录，避免删除设备上原有内容。

## 4. 启动与首次订阅

- **KUAL 入口：** 打开 KUAL，在 RSS Reader 菜单中选择 `RSS Reader`。
- **书库入口：** 已配置 Scriptlets 的设备，可在书库打开 `RSS Reader`，对应 `documents/RSS阅读器.sh`。没有 Scriptlets 时，复制脚本本身不会获得书库启动能力，使用 KUAL 入口即可。

首次没有订阅时，可以选择“添加订阅”，输入 RSS / Atom 的 HTTP(S) 地址，确定后同步。这里需要订阅源地址，不一定是网站首页地址；同步需要 Kindle 已连接可用网络。

已有 OPML 时：

1. 退出阅读器，连接 USB。
2. 把 OPML 文件命名为 `feeds.opml`，放到 `native-rss/data/feeds.opml`。`data` 不存在时可手动创建。
3. 安全弹出后打开应用，点击左上角标题进入“我的阅读”，选择“导入订阅”，再点击“同步”。

“导出 OPML”生成 `native-rss/data/subscriptions.opml`，只含订阅和分组。完整备份仍需保留 `data` 目录。

搜索、订阅名称和分组支持拼音输入：输入完整拼音后点候选词，再确定；ü 使用 v。已同步的文章可离线阅读，摘要型订阅不保证有完整正文。

## 5. 退出与保留数据升级

退出：返回文章列表，点击左上角标题进入“我的阅读”，选择“退出应用”。阅读器运行期间会暂时抑制 USB 磁盘导出，退出后恢复；若电脑看不到 Kindle 用户盘，先确认应用已退出。

升级时：

1. 退出应用，再备份设备的 `native-rss` 文件夹。
2. 下载新 Release 的 **KPW3 安装包**，按第 3 节合并覆盖程序文件。
3. 保留 `native-rss/data/`，不要先删除旧的 `native-rss` 目录。
4. 安全弹出并重新启动，检查订阅、收藏和阅读位置。

`v0.0.1` 安装包不带 `data` 目录，正确合并不会覆盖个人数据库。以后升级仍应先查看对应版本的迁移说明。

| 路径 | 内容 |
|---|---|
| `native-rss/data/rss.sqlite` | 订阅、缓存文章与图片、已读、收藏、阅读位置及设置 |
| `native-rss/data/feeds.opml` | 待导入的订阅列表 |
| `native-rss/data/subscriptions.opml` | 导出的订阅列表 |

备份数据库前先退出应用，最好直接复制整个 `data` 目录。数据库和 OPML 可能包含私人订阅地址，不要公开上传。

## 6. 常见问题

| 现象 | 检查方法 |
|---|---|
| KUAL 没有 RSS Reader | 检查 `extensions/native-rss/menu.json` 是否在正确层级，安全弹出后重新打开 KUAL |
| 书库没有入口或点击脚本无效 | 确认已配置 Scriptlets；未配置时通过 KUAL 启动 |
| 点击后返回、提示缺少工具 | 从 KUAL 执行“导出 RSS 诊断报告”，查看报告中的缺失工具、fbink 和密码锁状态 |
| 标题为空或没有文章 | 先导入/添加订阅，再联网同步；未读为空时切换“全部文章” |
| 同步失败 | 检查 Wi-Fi、订阅地址和订阅管理页错误；检查 `native-rss/certs/ca-bundle.pem` 是否完整复制 |
| 图片未缓存 | 重新同步；目前支持 JPEG、PNG、静态 WebP，不支持 GIF、动态 WebP、SVG 等 |
| 连接 USB 后电脑看不到用户盘 | 先从阅读器退出；应用运行期间 USB 磁盘导出被暂时抑制 |

诊断报告位于 `documents/RSS-device-report.txt`。可通过 KUAL 的“导出 RSS 诊断报告”生成；已配置 Scriptlets 时也可运行 `RSS设备诊断.sh`。诊断本身不接管屏幕或暂停系统进程。

反馈问题时说明机型、固件、Release 版本、启动入口、复现步骤，并附上检查过的诊断报告。不要通过强杀启动器或手动设置 `RSS_UI_GUARD` 来绕过启动检查。

## 7. 卸载

先退出应用并备份 `native-rss/data`，再移除 `extensions/native-rss`、`native-rss`、`documents/RSS阅读器.sh` 和 `documents/RSS设备诊断.sh`。仅删除这些应用自身文件，不要删除整个 `extensions` 或 `documents`。

删除 `native-rss/data` 会丢失本地文章与阅读记录。卸载本应用不会卸载 KUAL、Scriptlets 或其他扩展。
