# 安全反馈

当前为开发验证版，维护对象是 `main` 和最新开发 Release；尚未承诺旧版长期维护或响应时限。

RSS、OPML 和远程图片均是不可信输入。程序会联网访问用户配置的 HTTP(S) 地址，HTTPS 验证开启；订阅 URL 及其参数可能包含凭据。Kindle 启动器需要较高设备权限，并暂时接管系统界面，因此解析、下载、文件写入和生命周期恢复问题均应作为安全相关问题评估。

## 报告方式

如仓库 Security 页面提供 [Report a vulnerability](https://github.com/frevia/kindle-rss-reader/security/advisories/new)，请通过该入口私下提交受影响版本、最小复现、可能影响及脱敏后的日志。

若该入口尚未启用，请发送邮件至 [freviax@gmail.com](mailto:freviax@gmail.com)，附上受影响版本、最小复现和脱敏日志。不要在公开 Issue 中提交密钥、私人地址、数据库、完整 OPML 或可直接利用的攻击输入。

## 用户数据

应用没有服务器账号或遥测；订阅与图片请求会直接到达对应源站，源站仍可看到正常网络请求。数据库和 OPML 保存在 Kindle 用户盘，设备密码锁启用时启动器拒绝运行。运行日志可能包含请求错误及地址；公开前应人工检查。备份与卸载步骤见[安装教程](native-rss/docs/INSTALL.md)。

依赖版本与来源固定在 `native-rss/tools/arm-sources.json`、`native-rss/tools/runtime-sources.json` 和 `native-rss/third_party/*/SOURCE.json`。固定版本和哈希只验证来源一致性，不代表没有已知漏洞。
