# 启动问题排查

只有入口没出现或阅读器打不开时，才需要看这里。正常安装和使用见[安装教程](INSTALL.md)与[使用指南](USAGE.md)。

## KUAL 里没有阅读器

连接电脑，检查 Kindle 的 `extensions` → `native-rss` 中是否有 `menu.json`。如果没有，从安装包补复制 `extensions` 文件夹的内容。安全弹出并拔线，重新打开 KUAL，查看 **Kindle RSS → RSS Reader**。

## 点开后马上退出

1. 确认使用的是 Kindle Paperwhite 3（KPW3，2015 款），并在 Kindle 设置中关闭设备密码锁。
2. 对照[安装教程中的目录图](INSTALL.md#文件要放成这样)，确认文件位置正确，没有多套一层解压文件夹。
3. 检查下面的屏幕恢复工具；已经具备时，不要重复安装补丁，按[安装教程中的日志反馈步骤](INSTALL.md#日志反馈)提供检查文件。

### 检查屏幕恢复工具

当前启动器需要 FBInk 来恢复退出后的系统画面，安装包尚未附带它。KUAL 和书库两种入口都需要这个工具；能够打开其他第三方应用，不代表它一定已经存在。

1. 连接电脑，打开 Kindle 磁盘中的 `libkh` 文件夹，再打开 `bin`，查看里面是否有名为 **`fbink`** 的文件。
2. 文件已经存在时，先保留原文件。如果应用仍打不开，按[日志反馈步骤](INSTALL.md#日志反馈)提供检查文件，不要反复重装。
3. 如果书库里已有 **Run Hotfix**，安全弹出并拔线，点开运行，等处理完成。再连接电脑，检查 `libkh` → `bin` → `fbink` 是否已经出现。
4. 如果没有 **Run Hotfix**，或运行后仍没有这个文件，再看下方的补装步骤。

文件存在只是初步检查；是否可执行以及其他启动条件，仍需结合检查文件判断。

## 书库里没有 RSS Reader

1. 检查 Kindle 磁盘的 `documents` 中确实有 **`RSS阅读器.sh`**，并且 `native-rss` 文件夹也已完整复制。
2. 安全弹出并拔线，回到书库，把筛选切到 **全部**，不要只显示“书籍”，再等待列表刷新。
3. 如果刚装好 Hotfix，确认已经运行过书库里的 **Run Hotfix**。然后重新连接电脑，把安装包中的 `documents` 内容再复制一次，安全弹出并拔线。
4. 仍不出现时，在 Kindle 设置中选择**重新启动**，回到书库再看。这里是重启，不是恢复出厂设置。

书库入口依赖 Scriptlets / SH_Integration，它让书库能打开随包提供的 `.sh` 文件，不需要用户自己编写脚本。从 KUAL 打开不需要它，也不必同时准备两种入口。

如果其他 `.sh` 应用可以从书库打开，先核对本应用的文件位置；如果都不能，按下面的步骤检查越狱后的环境设置。已有可用的 KUAL 时，也可以尝试 **KUAL → Kindle RSS → RSS Reader**。

## 补装设备工具

**只有确认缺少 FBInk，或希望使用书库入口但缺少 Scriptlets 时，才需要补装。** Universal Hotfix 可以提供这两项工具；已经具备所需工具、阅读器能正常打开时，不必重复安装。

### 安装并运行补丁

以下步骤从“设备已越狱”开始。先退出正在运行的越狱应用，回到 Kindle 书库，并开启飞行模式，避免同时下载系统更新。

1. 在电脑上[下载 Universal Hotfix 补丁](https://github.com/KindleModding/Hotfix/releases/latest/download/Update_hotfix_universal.bin)。下载到的文件名应为 **`Update_hotfix_universal.bin`**，不要改名，也不用解压。下载链接失效时，到[补丁的最新版页面](https://github.com/KindleModding/Hotfix/releases/latest)找这个文件。
2. 连接 Kindle，把这个 `.bin` 文件复制到 Kindle 磁盘，**与 `documents` 文件夹并排放置**，不要放进 `documents` 或 `mrpackages`。
3. 如果这一层还有其他 `.bin` 安装文件，或 `update.bin.tmp.partial`，先移到电脑备份。本次只保留刚下载的 Hotfix 安装文件；不要混放其他系统更新文件。
4. 在电脑上安全弹出 Kindle，再拔掉数据线。
5. 打开 Kindle 的 **设置 → 全部设置 → 右上角三个点 → 更新 Kindle**；确认安装，等处理完成。不同界面的入口位置可能略有区别，找的是“更新 Kindle”这一项。
6. 回到书库，找到 **Run Hotfix**，点开运行。等它处理完并回到书库；**复制并安装 `.bin` 后，还要完成这一步，不能省略。**
7. 按[安装教程](INSTALL.md#安装)的步骤复制 RSS Reader 的三个文件夹，安全弹出并拔线。在书库中等待列表刷新，看到 **RSS Reader** 后点开。

这里使用“更新 Kindle”是为了安装 **Hotfix 补丁 `.bin`**；RSS Reader 安装包本身仍然只需要复制文件。无需下载 Amazon 的系统固件来安装本应用。

安装完成后，连接电脑确认 `libkh` → `bin` → `fbink` 已出现；想使用书库入口时，再确认能看到并打开 **RSS Reader**。补丁来源和安装步骤见[上游说明](https://github.com/KindleModding/kindlemodding.github.io/blob/main/content/jailbreaking/Legacy/post-jailbreak/setting-up-a-hotfix/__index.md)。

### “更新 Kindle”是灰色的，怎么办

先检查是否已拔掉 USB 数据线，以及 `Update_hotfix_universal.bin` 是否与 `documents` 并排放置。

如果之前在 KUAL 中用 **Rename OTA binaries** 关闭过更新，先保持飞行模式，打开该菜单并选择 **Restore**，再安装 Hotfix。补丁安装并运行完成后，回到同一菜单选择 **Rename**，恢复原来的更新阻止设置。没有使用过这个工具，就不要额外安装它。

仍然无法安装时，先停在这里，按上方的社区安装说明检查，或反馈设备型号和提示文字。不要通过升级系统或恢复出厂设置碰运气；已经可用的 KUAL 入口仍可继续使用。

更多启动条件和实现细节见[开发参考](DEVELOPMENT.md#维护设备启动环境)。
