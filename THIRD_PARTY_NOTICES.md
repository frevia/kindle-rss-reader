# 第三方组件与许可

阅读器用到了一些其他项目提供的代码、字体和词库。这份清单说明它们来自哪里、按什么规则使用。正常安装和阅读无需逐项处理；自己修改或再次分发应用时，再按下表核对完整许可。

项目原创代码与文档采用根目录 [MIT License](LICENSE)。下列第三方代码、数据、字体和证书保留各自许可，不因项目选择 MIT 而改变。此表用于导航，完整条款以随包许可及上游文件为准。源码链接指向在线仓库；仅有安装包时，可离线查看 `native-rss/licenses/` 中的完整条款。

## 随源码提供

| 组件 | 使用方式 | 许可与来源 |
|---|---|---|
| hbctool HBC84 格式表 | 离线首页补丁生成器只使用 opcode / structure 两张表；固定来源和校验值在 `native-rss/third_party/hbc84/SOURCE.json`，设备不运行 hbctool | MIT；`native-rss/third_party/hbc84/LICENSE`，安装包 `licenses/hbc84-MIT.txt` |
| stb_image | JPEG / PNG 解码，选择其 MIT 许可 | [LICENSE](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/stb/LICENSE)、[固定来源](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/stb/SOURCE.json) |
| Rime pinyin-simp | 原始词库及生成的只读候选表；生成时合并音节空格并按词频排序 | Apache-2.0；[LICENSE](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/pinyin/LICENSE)、[固定来源](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/pinyin/SOURCE.json)；上游词库注明源自 Android PinyinIME |

## CREngine-ng 正文排版与组合程序

v0.0.3 使用 [CREngine-ng 0.9.13](https://gitlab.com/coolreader-ng/crengine-ng/-/tree/ef51353d6afecc924b639ed9c8c57c5b5faa6e5d)，不再链接旧 KOReader CREngine 分支。固定归档、校验值及许可依据见 `native-rss/tools/reader-sources.json` 与 `native-rss/third_party/crengine/SOURCE.json`。上游 README 和源文件明确为 GPL-2.0-or-later，本构建选择 GPLv3。

组合 `kindle-rss` 程序按根目录 [GPLv3](COPYING) 分发，项目原创源码继续保留 MIT；单独的首页后台和脚本保留原许可。GPLv3 与所用 Apache-2.0 组件的组合依据见 [Apache 官方说明](https://www.apache.org/licenses/GPL-compatibility.html)。此调整不改变历史版本的许可。

安装包包含 GPLv3、上游 GPLv2、README、作者名单和 qimagescale 原始声明。qimagescale 按其 LGPLv3 路径用于 GPLv3 组合，原有 Imlib2 声明保留。已禁用与 RSS HTML/PNG 阅读无关的 CHM、antiword、Markdown、SVG 等模块，不分发它们的可执行代码。

每次 Release 同时提供项目源码、第三方对应源码归档和 SHA-256。后者收录全部构建锁定的原始依赖归档，以及包含 musl、libc++、libc++abi、libunwind 和 compiler-rt 源码的固定 Zig 工具链。项目源码包含拼音原始词库、生成器、引擎构建修改及安装脚本。完整范围与重建步骤见[发布许可核对](native-rss/docs/RELEASE_LICENSE_AUDIT.md)。

## ARM 安装包组件

版本、下载地址与校验值见 [arm-sources.json](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/tools/arm-sources.json) 和 [runtime-sources.json](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/tools/runtime-sources.json)。完整许可放入安装包的 `native-rss/licenses/`；源码开发包也包含同一组许可文件。

| 组件 | 许可 / 随包文件 |
|---|---|
| CREngine-ng | GPL-2.0-or-later，本构建选择 GPLv3；`CREngine-GPL-2.0.txt`、`GPL-3.0.txt`、`CREngine-README.txt`、`CREngine-AUTHORS.txt`、`CREngine-qimagescale*.txt` |
| HarfBuzz | MIT类许可及上游版权说明；`HarfBuzz.txt` |
| libpng | PNG Reference Library License；`libpng.txt` |
| libunibreak | zlib类许可；`libunibreak.txt` |
| xxHash | BSD-2-Clause；`xxHash.txt` |
| OpenSSL | Apache-2.0；`OpenSSL.txt` |
| curl | curl license；`curl.txt` |
| libxml2 | MIT 类许可及上游版权说明；`libxml2.txt` |
| zlib | zlib license；`zlib.txt` |
| SQLite | 公有领域声明；`SQLite.txt` |
| FreeType | 选择 FreeType License；`FreeType*.txt` 保留 FTL 与 BDF / PCF 声明 |
| libwebp | BSD 类许可；`libwebp.txt`、`libwebp-PATENTS.txt` |
| libavif | BSD-2-Clause；`libavif.txt` |
| libaom | BSD-2-Clause 及专利声明；`libaom.txt`、`libaom-PATENTS.txt` |
| libyuv | BSD-3-Clause 及专利声明；`libyuv.txt`、`libyuv-PATENTS.txt` |
| musl | MIT 类许可及组件声明；`musl.txt` |
| libc++ / libc++abi / libunwind | Apache-2.0 WITH LLVM-exception 及文件内其他声明；对应的 `libcxx*.txt`、`libunwind.txt` |
| Zig 工具链运行时代码 | MIT 及工具链组件原许可；`Zig.txt` 与上述运行库声明 |
| Noto Sans SC | SIL OFL 1.1；未修改字体，`Noto-OFL.txt` |
| Mozilla 根证书集合（经 curl.se 获取） | MPL-2.0；`Mozilla-MPL-2.0.txt`，来源记录于 `runtime-sources.json` |

Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved.

macOS 的系统字体仅在本机读取，不随项目分发。SDL2 仅用于宿主桌面窗口，采用 zlib 许可；CMake 经 pkg-config 使用宿主安装的 SDL2，KPW3 安装包不包含它。

## 设备外部依赖

启动入口可选择 KUAL 或 Scriptlets，无需同时安装；Scriptlets 仅用于书库图标入口，KUAL 启动不需要它。两种入口共用的 RSS 启动器仍需要 FBInk。这些外部程序不包含在 RSS 安装包中；Universal Hotfix 是 Scriptlets 和 FBInk 的一种提供方式，已有可用环境无需重装。启动器调用 `/mnt/us/libkh/bin/fbink` 命令进行系统界面恢复刷新，没有链接 FBInk 库。FBInk 上游采用 [GPL-3.0-or-later](https://github.com/NiLuJe/FBInk)；若以后将其二进制或其他外部工具加入安装包，必须先核对许可和对应源码分发要求。

设备 ABI 与系统服务参考来源见 [DEVICE.md](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/docs/DEVICE.md)。本项目不包含 Kindle 固件或 Amazon 系统组件。订阅文章和图片的权利归各自权利人，MIT 许可不授予对这些内容的再发布权利。
