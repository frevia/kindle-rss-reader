# 参与开发

欢迎通过 [Issues](https://github.com/frevia/kindle-rss-reader/issues) 报告问题或讨论改进。当前重点是 KPW3 的安装、阅读和生命周期可靠性。较大的界面调整、设备适配或数据库变更，请先说明使用场景和预期行为。

## 反馈问题

请提供 Release 版本或提交号、设备型号、固件版本、KUAL / Scriptlets 启动入口、复现步骤、预期与实际结果。安装问题先按[安装教程](native-rss/docs/INSTALL.md)检查目录与依赖。

诊断报告会包含系统信息与运行日志，提交前检查并遮盖私人订阅 URL、查询参数、令牌、IP、序列号及其他个人信息。不要上传整个数据库、OPML 或未经检查的原始报告。使用 `example.invalid` 等虚构地址构造最小样例。安全问题按 [SECURITY.md](SECURITY.md)处理。

## 构建与验证

从仓库根目录操作；依赖、宿主构建、ARM 构建和打包步骤见[开发文档](native-rss/README.md)。修改解析、存储、网络或交互行为时，补充相应的最小回归测试。

```sh
rtk proxy cmake -S native-rss -B native-rss/build -DCMAKE_BUILD_TYPE=Debug
rtk proxy cmake --build native-rss/build -j 6
rtk proxy ctest --test-dir native-rss/build --output-on-failure
rtk git diff --check
```

这些命令中的 `rtk proxy` 是本仓库的命令代理；未安装 RTK 的外部贡献者可直接运行后面的 `cmake`、`ctest` 和 `git` 命令。

宿主测试使用临时数据，不连接 Kindle。设备、启动器或系统恢复相关变更需要单独记录实机验证，注明机型、固件与未覆盖的场景。测试通过不能替代墨水屏、触摸和耗电验收。

## 提交变更

- 每个 PR 聚焦一个问题，说明行为变化、验证结果和已知限制；涉及数据库时说明备份、迁移和兼容性。
- 保留现有数据与诊断产物的忽略规则；不要提交 `vendor`、`build*`、`output`、个人数据库或设备原始报告。
- 提交标题采用 Conventional Commits，并写清中文业务语义，例如 `fix: 修复订阅分组移动后的筛选范围`；正文说明主要内容、接口或数据影响及验证方式。
- 提交自己有权授权的内容；项目原创部分采用 [MIT](LICENSE)，第三方材料保留原许可与来源，见[第三方说明](THIRD_PARTY_NOTICES.md)。
