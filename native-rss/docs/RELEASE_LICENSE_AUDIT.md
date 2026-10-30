# v0.0.3 许可与对应源码

核对日期：2026-10-30。本文记录本项目选择的分发方式和可复核材料，不代替上游许可全文。

## 引擎来源

原开发构建使用 KOReader CREngine 的 `f335f7a97e240f505bb8a0dfb721fcb7912a7824`，README 只写 GPL version 2。CoolReader 在 [issue #338](https://github.com/buggins/coolreader/issues/338) 讨论了这种声明的歧义，并通过 [PR #339](https://github.com/buggins/coolreader/pull/339) 澄清为 GPLv2-or-later、整理作者并移除部分来源不明代码。不能把这次改动未经核对地套用到另一分支。

本版本实际切换为 CREngine-ng 0.9.13，固定提交 `ef51353d6afecc924b639ed9c8c57c5b5faa6e5d`。其 [README](https://gitlab.com/coolreader-ng/crengine-ng/-/blob/ef51353d6afecc924b639ed9c8c57c5b5faa6e5d/README.md) 与源文件明确允许 GPLv2 或后续版本；[ChangeLog](https://gitlab.com/coolreader-ng/crengine-ng/-/blob/ef51353d6afecc924b639ed9c8c57c5b5faa6e5d/ChangeLog) 记录 2022 年的许可澄清。原始 README、LICENSE、AUTHORS 随项目保存，摘要记录于 `third_party/crengine/SOURCE.json`。

## 分发选择

- 组合 `kindle-rss` 可执行程序采用 GPL-3.0-only，完整条款为根目录 `COPYING`。项目原创源码仍保留 `LICENSE` 中的 MIT 授权，第三方代码不被重新标为 MIT。
- 引擎依据 GPL-2.0-or-later 选择 GPLv3；qimagescale 保留原始声明，按其 LGPLv3 路径纳入 GPLv3 组合。
- OpenSSL 和拼音数据保留 Apache-2.0，依据 [Apache 官方兼容性说明](https://www.apache.org/licenses/GPL-compatibility.html) 与 GPLv3 组合。
- FreeType 选择 FTL；其他库、字体、证书、运行库和工具按[第三方清单](../../THIRD_PARTY_NOTICES.md)保留条款。
- 首页后台、启动脚本和可选本机固件差分适配单独保留原许可与安装边界。公开包不包含 Amazon 固件、个人数据库或原始设备报告。

## 对应源码附件

发布包含三份 ZIP 和一个 `SHA256SUMS.txt`：

1. `-KPW3.zip`：安装文件、完整许可、构建记录与逐文件哈希。
2. `-source.zip`：应用源码、原始拼音词库、生成器、构建锁、脚本、测试及文档。
3. `-third-party-sources.zip`：构建锁中的每个依赖的原始归档，以及固定 Zig 工具链归档。工具链内含实际链接的 musl、libc++、libc++abi、libunwind、compiler-rt 源码，也包含复现所需的编译器程序。

第三方归档的 `MANIFEST.json` 记录来源、版本、锁摘要、原文件字节摘要和上游校验方式。libyuv 的 Gitiles 归档带动态时间戳，使用锁定的 `tar-content-v1` 内容校验；附件中的实际字节另有 SHA-256。验证时检查精确成员集合，拒绝缺失、额外、重复、被修改或与当前锁不符的内容。

引擎的本地改动由 `tools/build_reader.py` 重现：将上游顶层 CMake 文件中的根路径改为项目路径，以便作为子项目编译；修改前后均固定 SHA-256。上游源归档保持原样，集成脚本及其源码随项目提供。RSS 仅启用 HTML 阅读需要的字体、排版和 PNG 支持，其余已缓存图片由 RSS 转换为 PNG 交给引擎。

## 重建

保存同一 Release 的两份源码包，先核对 `SHA256SUMS.txt`。解开项目源码和第三方归档后，在项目的 `native-rss` 目录执行以下步骤（Apple Silicon macOS，宿主依赖见[开发参考](DEVELOPMENT.md)）：

1. 将第三方归档中各个 `archives/` 成员按其 `MANIFEST.json` 中 URL 的最后一段文件名复制到 `vendor/arm-sources/archives/`。构建脚本仍会按锁重新核验，不能跳过校验。
2. 将 Zig 归档解压到 `vendor/`，得到 `vendor/zig-aarch64-macos-0.15.2/zig`。
3. 运行 `rtk proxy python3 tools/build_arm.py` 重建 ARM 程序；运行 `rtk proxy python3 tools/release.py v0.0.3` 生成并核验安装包及两份源码包。

普通构建会按 `runtime-sources.json` 获取字库和证书。离线重建时，可从同一安装包的 `fonts/reader.ttf`、`certs/ca-bundle.pem` 及 `licenses/Noto-OFL.txt`、`licenses/Mozilla-MPL-2.0.txt` 复制到 `vendor/runtime/`，分别命名为 `reader.ttf`、`ca-bundle.pem`、`OFL.txt`、`MPL-2.0.txt`；脚本同样会校验。

`device/launch.sh`、`device/home.sh` 与安装指南包含运行和安装所需步骤；RSS 不要求签名密钥。设备需已越狱并具备说明中的外部运行环境。
