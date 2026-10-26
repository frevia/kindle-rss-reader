# RSS Reader 安装说明

适用于**已越狱的 Kindle Paperwhite 3（KPW3，2015 款）**。其他机型暂未验证。开始前，请在 Kindle 设置中关闭设备密码锁；阅读器目前不能在开启密码锁时运行。

## 先选一种打开方式

| 你想怎样打开 | 需要准备什么 |
|---|---|
| 从 KUAL 菜单打开 | 已有可用的 KUAL 即可使用这个入口，**不需要 Scriptlets**。 |
| 在书库里点图标打开 | 需要 **Scriptlets / SH_Integration**。没有时，按下方的“准备缺少的组件”操作。 |

选择一种入口就够了，不需要把两套启动工具都装上。已经能正常打开 RSS Reader 时，也不需要为它重装 Hotfix。

**两种打开方式都需要屏幕恢复工具 FBInk。** 当前安装包没有附带它，首次安装时请按下面检查：

1. 连接电脑，打开 Kindle 磁盘中的 `libkh` 文件夹，再打开 `bin`，查看里面是否有名为 **`fbink`** 的文件。
2. 文件已经存在时，先保留原文件，继续安装阅读器。文件夹或文件不存在时，需要补齐这个工具。
3. 如果书库里已有 **Run Hotfix**，安全弹出并拔线，点开运行，等处理完成。再连接电脑，检查 `libkh` → `bin` → `fbink` 是否已经出现。
4. 如果没有 **Run Hotfix**，或运行后仍没有这个文件，按下方的 **准备缺少的组件** 安装补丁，完成后再检查同一位置。

文件补齐后，再从你选的入口打开 RSS Reader。文件已存在但应用仍打不开时，按文末步骤导出检查文件，进一步定位原因。

## 安装

1. 在电脑上打开[最新版下载页面](https://github.com/frevia/kindle-rss-reader/releases/latest)，展开 **Assets（下载文件）**，下载名称以 **`-KPW3.zip`** 结尾的安装包并解压。
2. 用数据线连接 Kindle，把解压后的 **`native-rss`、`documents`、`extensions`** 三个文件夹，合并复制到 Kindle 磁盘中。复制到的位置，就是打开磁盘后能直接看到 `documents` 的那一层。
3. 安全弹出并拔掉数据线。使用 KUAL 的用户，点 **KUAL → Kindle RSS → RSS Reader**；已准备好书库启动的用户，在书库点 **RSS Reader**。首次使用连接 Wi-Fi，再按[使用指南](USAGE.md)添加订阅、下载文章。

使用 KUAL 且已备好 FBInk 时，可以跳过后面的组件安装。想从书库打开时，还需备好 Scriptlets。

名字里带 `source` 或 `Source code` 的是源码，不能用来安装。`SHA256SUMS.txt` 是下载校验文件，正常安装不需要打开。

### 文件要放成这样

```text
Kindle 磁盘
├── documents
│   ├── 原有的书籍……
│   ├── RSS阅读器.sh
│   └── RSS设备诊断.sh
├── extensions
│   └── native-rss
└── native-rss
    ├── bin
    ├── fonts
    ├── certs
    └── 其他随包文件……
```

不要先删除 Kindle 上原有的 `documents` 或 `extensions`。遇到同名文件夹请选择“合并”；如果电脑只提供“替换整个文件夹”，请取消，再打开同名文件夹，分别复制里面的内容。升级时也要保留 `native-rss` 中的 `data` 文件夹。

不要多套一层解压文件夹，不要只把 ZIP 放进 Kindle。**RSS Reader 的 ZIP 用复制文件的方式安装，不用“更新 Kindle”。**

## 准备缺少的组件

**缺少 FBInk，或想使用书库图标但还没有 Scriptlets 时，才需要看这一节。** Scriptlets 让应用出现在书库里。RSS Reader 已提供启动文件，不需要自己编写或修改脚本。

Universal Hotfix 补丁可以准备这两项组件。已经具备所需组件时，不必重复安装：

- 从 KUAL 打开：确认 FBInk 已存在，就可以使用这个入口。
- 从书库打开：确认 FBInk 已存在，并且书库已能打开 `.sh` 应用，就可以使用这个入口。
- 书库中已有 **Run Hotfix** 但还未运行：先运行一次，再按前面的路径检查 FBInk，并查看书库入口。

### 安装并运行补丁

以下步骤从“设备已越狱”开始。先退出正在运行的越狱应用，回到 Kindle 书库，并开启飞行模式，避免同时下载系统更新。

1. 在电脑上[下载 Universal Hotfix 补丁](https://github.com/KindleModding/Hotfix/releases/latest/download/Update_hotfix_universal.bin)。下载到的文件名应为 **`Update_hotfix_universal.bin`**，不要改名，也不用解压。下载链接失效时，到[补丁的最新版页面](https://github.com/KindleModding/Hotfix/releases/latest)找这个文件。
2. 连接 Kindle，把这个 `.bin` 文件复制到 Kindle 磁盘，**与 `documents` 文件夹并排放置**，不要放进 `documents` 或 `mrpackages`。
3. 如果这一层还有其他 `.bin` 安装文件，或 `update.bin.tmp.partial`，先移到电脑备份。本次只保留刚下载的 Hotfix 安装文件；不要混放其他系统更新文件。
4. 在电脑上安全弹出 Kindle，再拔掉数据线。
5. 打开 Kindle 的 **设置 → 全部设置 → 右上角三个点 → 更新 Kindle**；确认安装，等处理完成。不同界面的入口位置可能略有区别，找的是“更新 Kindle”这一项。
6. 回到书库，找到 **Run Hotfix**，点开运行。等它处理完并回到书库；**复制并安装 `.bin` 后，还要完成这一步，不能省略。**
7. 按本文开头的“安装”步骤复制 RSS Reader 的三个文件夹，安全弹出并拔线。在书库中等待列表刷新，看到 **RSS Reader** 后点开。

这里使用“更新 Kindle”是为了安装 **Hotfix 补丁 `.bin`**；RSS Reader 安装包本身仍然只需要复制文件。无需下载 Amazon 的系统固件来安装本应用。

安装完成后，连接电脑确认 `libkh` → `bin` → `fbink` 已出现；想使用书库入口时，再确认能看到并打开 **RSS Reader**。补丁来源和安装步骤见[上游说明](https://github.com/KindleModding/kindlemodding.github.io/blob/main/content/jailbreaking/Legacy/post-jailbreak/setting-up-a-hotfix/__index.md)。

### “更新 Kindle”是灰色的，怎么办

先检查是否已拔掉 USB 数据线，以及 `Update_hotfix_universal.bin` 是否与 `documents` 并排放置。

如果之前在 KUAL 中用 **Rename OTA binaries** 关闭过更新，先保持飞行模式，打开该菜单并选择 **Restore**，再安装 Hotfix。补丁安装并运行完成后，回到同一菜单选择 **Rename**，恢复原来的更新阻止设置。没有使用过这个工具，就不要额外安装它。

仍然无法安装时，先停在这里，按上方的社区安装说明检查，或反馈设备型号和提示文字。不要通过升级系统或恢复出厂设置碰运气；已经可用的 KUAL 入口仍可继续使用。

### 文件复制了，书库里仍没有图标

1. 检查 Kindle 磁盘的 `documents` 中确实有 **`RSS阅读器.sh`**，并且 `native-rss` 文件夹也已完整复制。
2. 安全弹出并拔线，回到书库，把筛选切到 **全部**，不要只显示“书籍”，再等待列表刷新。
3. 如果刚装好 Hotfix，确认已经运行过书库里的 **Run Hotfix**。然后重新连接电脑，把安装包中的 `documents` 内容再复制一次，安全弹出并拔线。
4. 仍不出现时，在 Kindle 设置中选择**重新启动**，回到书库再看。这里是重启，不是恢复出厂设置。

如果其他 `.sh` 应用也没有入口，先检查 Hotfix 是否完成安装和运行；如果只有 RSS Reader 没有入口，先检查文件是否放对位置。

## 升级

1. 在阅读器中点 **RSS Reader → 退出**，确认回到 Kindle 系统书库。连接电脑，把整个 `native-rss` 文件夹复制到电脑备份。
2. 下载最新版，按“安装”步骤合并覆盖文件，**保留 `native-rss/data`**。安装包不带这个文件夹，里面保存你的订阅、收藏和阅读记录。
3. 安全弹出并拔线，再次打开 RSS Reader，确认原有数据还在。升级阅读器本身无需重装 Scriptlets 或 Hotfix。

## 卸载

1. 正常退出阅读器；如果还想保留阅读数据，先把整个 `native-rss` 文件夹备份到电脑。
2. 删除 Kindle 上的 `native-rss`、`extensions` 里的 `native-rss`，以及 `documents` 里的 `RSS阅读器.sh` 和 `RSS设备诊断.sh`。

只删除本应用的文件，不要删除整个 `documents` 或 `extensions`，也不要删除其他应用的启动文件。卸载本应用不需要卸载共享的 Hotfix、Scriptlets 或 KUAL。删除 `native-rss/data` 会丢失本地阅读数据。

## 打不开或用不了怎么办

| 遇到的情况 | 先这样做 |
|---|---|
| KUAL 里没有阅读器 | 检查 Kindle 的 `extensions` → `native-rss` 中有 `menu.json`，安全弹出后重新打开 KUAL。 |
| 点开后马上退出 | 先检查设备密码锁和文件位置。缺少 FBInk 时，按本文开头的检查和补装步骤处理；文件已存在时，再导出检查文件。 |
| 打开后没有文章 | 先添加订阅并同步；以前的文章可以在“全部”中查看。 |
| 下载失败 | 检查 Wi-Fi，以及填入的是否是网站提供的订阅地址。 |
| 图片没有显示 | 先重新同步。部分图片格式、特别大的图片或网站限制可能导致图片无法下载，不影响已经下载的文字。 |
| 电脑看不到 Kindle 磁盘 | 先从阅读器里退出，再重新连接，并确认数据线能传文件。 |

### 日志反馈

1. 在书库打开 **RSS设备诊断**，或在 **KUAL → Kindle RSS** 中选择 **导出 RSS 诊断报告**。
2. 连接电脑，在 `documents` 中找到 **`RSS-device-report.txt`**。
3. 到[问题反馈页面](https://github.com/frevia/kindle-rss-reader/issues)提交问题，说明设备型号、安装包文件名、故障时间，以及做了什么操作、看到了什么。系统版本能找到时也请一并提供。

公开发送前先检查报告，遮住私人订阅地址等内容，不要上传整个 `data` 文件夹或完整订阅列表。拿不准时，可先邮件联系 [freviax@gmail.com](mailto:freviax@gmail.com)。
