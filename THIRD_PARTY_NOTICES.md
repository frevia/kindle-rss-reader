# 第三方组件与许可

阅读器用到了一些其他项目提供的代码、字体和词库。这份清单说明它们来自哪里、按什么规则使用。正常安装和阅读无需逐项处理；自己修改或再次分发应用时，再按下表核对完整许可。

项目原创代码与文档采用根目录 [MIT License](LICENSE)。下列第三方代码、数据、字体和证书保留各自许可，不因项目选择 MIT 而改变。此表用于导航，完整条款以随包许可及上游文件为准。源码链接指向在线仓库；仅有安装包时，可离线查看 `native-rss/licenses/` 中的完整条款。

## 随源码提供

| 组件 | 使用方式 | 许可与来源 |
|---|---|---|
| stb_image | JPEG / PNG 解码，选择其 MIT 许可 | [LICENSE](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/stb/LICENSE)、[固定来源](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/stb/SOURCE.json) |
| Rime pinyin-simp | 原始词库及生成的只读候选表；生成时合并音节空格并按词频排序 | Apache-2.0；[LICENSE](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/pinyin/LICENSE)、[固定来源](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/third_party/pinyin/SOURCE.json)；上游词库注明源自 Android PinyinIME |

## CREngine 正文排版

当前开发构建已在正式RSS入口接入 [KOReader CREngine](https://github.com/koreader/crengine)，固定来源见 `native-rss/tools/reader-sources.json` 和 `native-rss/third_party/crengine/SOURCE.json`。上游 README.TXT 标注 GPL version 2；完整许可随包保存为 `CREngine-GPL-2.0.txt`，qimagescale 的其他声明另存。

项目原创源码继续保留 MIT，但此声明不能覆盖含CREngine的组合二进制。现有OpenSSL和拼音词库含Apache-2.0组件，组合许可兼容尚未确认；当前仅生成本地开发安装包，`public_release_ready=false`，发布工具拒绝将其生成公开Release附件。此说明不改变已发布的原排版版本。上游声明及组件关系需要在公开分发前完成核对，不以附上许可文本代替兼容性审查。

构建脚本校验每个源码归档，源码包包含构建脚本、锁文件及RSS集成代码；本地安装包没有声称满足GPL组合产物的完整公开源码分发要求。

## ARM 安装包组件

版本、下载地址与校验值见 [arm-sources.json](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/tools/arm-sources.json) 和 [runtime-sources.json](https://github.com/frevia/kindle-rss-reader/blob/main/native-rss/tools/runtime-sources.json)。完整许可放入安装包的 `native-rss/licenses/`；源码开发包也包含同一组许可文件。

| 组件 | 许可 / 随包文件 |
|---|---|
| CREngine | 上游标注GPLv2；`CREngine-GPL-2.0.txt`、`CREngine-README.txt`、`CREngine-qimagescale.txt` |
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
