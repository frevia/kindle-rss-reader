#!/bin/sh
# Optional homepage adapter. Normal application installation never calls enable.
umask 077
APP=/mnt/us/native-rss
STATE=/var/local/native-rss/home
REPORTS=/mnt/us/native-rss-backups/home-restart-reports
TARGET=/app/KPPMainApp/js/KPPMainApp.js.hbc
JOB=/etc/upstart/native-rss-home.conf
INIT=/sbin/initctl
MNTROOT=/usr/sbin/mntroot
GET=/usr/bin/lipc-get-prop
SET=/usr/bin/lipc-set-prop
PROC=/proc
SELF=/mnt/us/native-rss/home.sh
ROOT_RO=0

fail() { printf '首页 RSS：%s\n' "$*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
private_state() {
    [ ! -L /var/local/native-rss ] && [ ! -L "$STATE" ] || fail '状态目录不能是符号链接'
    mkdir -p "$STATE" || fail '无法创建状态目录'
    case "$(stat -c '%a:%u' "$STATE")" in 700:0|2700:0) ;; *) fail '状态目录必须属于 root 且仅 root 可访问' ;; esac
}
hex_value() {
    value=$(awk -F= -v key="$1" '$1 == key {print $2}' "$APP/home/profile.env") || return 1
    [ "${#value}" -eq 64 ] || return 1
    case "$value" in *[!a-f0-9]*) return 1 ;; esac
    printf '%s\n' "$value"
}
profile() {
    [ -f "$APP/home/profile.env" ] && [ ! -L "$APP/home/profile.env" ] || fail '未准备本机固件补丁'
    ORIGINAL=$(hex_value ORIGINAL_SHA256) || fail '无效原固件校验值'
    PATCHED=$(hex_value PATCHED_SHA256) || fail '无效补丁输出校验值'
    PATCH=$(hex_value PATCH_SHA256) || fail '无效差分补丁校验值'
    case "$ORIGINAL" in 999d9234f5557820ce13526fa7732e2bebe273a84f5a375c97f9a1936d4cafa2) ;; *) fail '固件不受支持' ;; esac
    [ -f "$APP/home/patch.rhp" ] && [ ! -L "$APP/home/patch.rhp" ] || fail '缺少差分补丁'
    [ "$(hash "$APP/home/patch.rhp")" = "$PATCH" ] || fail '差分补丁已改变'
}
mounted() { awk -v target="$TARGET" '$2 == target {found=1} END {exit !found}' "$PROC/mounts"; }
prepare_copy() {
    profile
    [ -f "$TARGET" ] && [ ! -L "$TARGET" ] || fail '原首页文件异常'
    [ "$(hash "$TARGET")" = "$ORIGINAL" ] || fail '固件哈希不匹配，保留原首页'
    for name in original.hbc patched.hbc mounted.sha256; do [ ! -L "$STATE/$name" ] || fail '状态文件不能是符号链接'; done
    bytes=$(stat -c '%s' "$TARGET") || fail '无法读取首页文件大小'
    # Keep room for the reader framebuffer snapshot.
    required=2048
    [ -f "$STATE/original.hbc" ] || required=$((required + (bytes + 1023) / 1024))
    if [ ! -f "$STATE/patched.hbc" ] || [ "$(hash "$STATE/patched.hbc")" != "$PATCHED" ]; then
        required=$((required + (bytes + 65536 + 1023) / 1024))
    fi
    available=$(df -Pk "$STATE" | awk 'NR == 2 {print $4}')
    case "$available" in ''|*[!0-9]*) fail '无法读取状态分区剩余空间' ;; esac
    [ "$available" -ge "$required" ] || fail '状态分区空间不足，保留原首页'
    if [ ! -f "$STATE/original.hbc" ]; then
        cp -p "$TARGET" "$STATE/original.hbc" || fail '无法备份原首页'
        chmod 600 "$STATE/original.hbc" || fail '无法保护原首页备份'
    fi
    [ "$(hash "$STATE/original.hbc")" = "$ORIGINAL" ] || fail '原首页备份校验失败'
    if [ ! -f "$STATE/patched.hbc" ] || [ "$(hash "$STATE/patched.hbc")" != "$PATCHED" ]; then
        timeout 15 "$APP/bin/kindle-rss-home" patch "$TARGET" "$APP/home/patch.rhp" "$STATE/patched.hbc" || fail '无法生成首页适配副本'
    fi
    [ "$(hash "$STATE/patched.hbc")" = "$PATCHED" ] || fail '首页适配副本校验失败'
    chmod 644 "$STATE/patched.hbc" || fail '无法设置首页副本读取权限'
}
mount_home() {
    private_state
    [ ! -L "$STATE/mount.lock" ] || fail '无效挂载锁'
    exec 7>"$STATE/mount.lock"; flock -n 7 || fail '挂载操作进行中'
    if mounted; then
        [ -f "$STATE/mounted.sha256" ] && [ "$(hash "$TARGET")" = "$(cat "$STATE/mounted.sha256")" ] || fail '已有未知首页挂载'
        return 0
    fi
    prepare_copy
    # Record ownership before mounting, so recovery can recognize an interrupted mount.
    printf '%s\n' "$PATCHED" >"$STATE/mounted.sha256"
    mount -o bind "$STATE/patched.hbc" "$TARGET" || fail '无法挂载首页副本'
}
unmount_home() {
    private_state
    [ ! -L "$STATE/mount.lock" ] || fail '无效挂载锁'
    exec 7>"$STATE/mount.lock"; flock -n 7 || fail '挂载操作进行中'
    if mounted; then
        [ -f "$STATE/mounted.sha256" ] && [ ! -L "$STATE/mounted.sha256" ] || fail '首页挂载归属不明'
        [ "$(hash "$TARGET")" = "$(cat "$STATE/mounted.sha256")" ] || fail '保留未知首页挂载'
        # KPPMainApp memory-maps this file; detach the owned bind before reloading it.
        umount -l "$TARGET" || fail '无法卸载首页副本'
    fi
    rm -f "$STATE/mounted.sha256"
}
root_rw() {
    if awk '$2 == "/" && $1 == "/dev/root" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' "$PROC/mounts"; then
        : >"$STATE/root-was-ro" || fail '无法保存根分区恢复状态'
        timeout 10 "$MNTROOT" rw >/dev/null || fail '无法临时写入启动配置'
        ROOT_RO=1
    fi
}
root_restore() {
    if [ "$ROOT_RO" -eq 1 ] || [ -f "$STATE/root-was-ro" ]; then
        if timeout 10 "$MNTROOT" ro >/dev/null; then
            ROOT_RO=0; rm -f "$STATE/root-was-ro"
        else printf '%s\n' '首页 RSS：无法恢复根分区只读状态' >&2; return 1; fi
    fi
}
owned_job() { [ ! -L "$JOB" ] && [ "$(head -n 1 "$JOB" 2>/dev/null)" = '# RSS Reader optional home card v1' ]; }
bridge_ready() {
    [ -f "$STATE/health" ] && [ ! -L "$STATE/health" ] || return 1
    grep -qx 'ready=1' "$STATE/health" || return 1
    bridge_pid=$(timeout 3 "$INIT" status native-rss-home 2>/dev/null | awk '/start\/running, process/ {print $NF}')
    case "$bridge_pid" in ''|*[!0-9]*) return 1 ;; esac
    grep -qx "pid=$bridge_pid" "$STATE/health" && [ "$(cat "$PROC/$bridge_pid/comm" 2>/dev/null)" = kindle-rss-home ]
}
main_pid() { timeout 3 "$INIT" status kppmainapp 2>/dev/null | awk '/start\/running, process/ {print $NF}'; }
archive_report() {
    case "$1" in ''|*[!0-9]*) return ;; esac
    [ ! -L /mnt/us/native-rss-backups ] && [ ! -L "$REPORTS" ] || return
    backup=$REPORTS
    mkdir -p "$backup" || return
    for report in /mnt/us/documents/KPPMainAppV2_"$1"_*; do
        [ -e "$report" ] && [ ! -L "$report" ] || continue
        case "$report" in *.txt|*.tgz|*.sdr) mv "$report" "$backup/" || : ;; esac
    done
    [ "${2:-}" = quiet ] && return 0
    timeout 3 "$SET" com.lab126.pillow pillowAlert '{"name":"simple_alert","clientParams":"{\"alertId\":\"collectingDebugInfoAlert\",\"hide\":true}"}' >/dev/null 2>&1 || :
}
reload_home() {
    previous=$(main_pid)
    timeout 25 "$INIT" restart kppmainapp >/dev/null || return 1
    if [ "$(timeout 2 "$GET" com.lab126.powerd state 2>/dev/null)" = active ]; then
        timeout 5 "$SET" com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || :
    fi
    archive_report "$previous"
    # The firmware may finish its restart report after the new home has become ready.
    nohup sh "$SELF" archive-after "$previous" 7>&- 8>&- 9>&- </dev/null >/dev/null 2>&1 &
}
disable_home() {
    changed=0
    if [ -e "$JOB" ] || [ -L "$JOB" ]; then
        owned_job || fail '启动配置不属于 RSS，拒绝修改'
        root_rw
        rm "$JOB" || fail '无法移除启动配置'
        timeout 5 "$INIT" reload-configuration || fail '无法重载启动配置'
        root_restore || fail '无法恢复根分区只读状态'
        changed=1
    fi
    if [ -f "$STATE/installing-job" ]; then
        [ ! -L "$JOB.rss-tmp" ] || fail '临时启动配置不是本组件的普通文件'
        root_rw
        rm -f "$JOB.rss-tmp" || fail '无法清理中断的启动配置'
        rm -f "$STATE/installing-job"
        root_restore || fail '无法恢复根分区只读状态'
    fi
    # The post-stop hook also unmounts; the explicit call covers missing jobs.
    timeout 20 "$INIT" stop native-rss-home >/dev/null 2>&1 || :
    if mounted; then changed=1; sh "$SELF" unmount || fail '无法恢复原首页挂载'; fi
    rm -f "$STATE/enabled"
    if [ "$changed" -eq 1 ]; then reload_home || fail '原首页已恢复，但需要重新加载界面'; fi
    printf '%s\n' '首页 RSS 已停用，原首页文件未改写'
}
enable_home() {
    [ "$(timeout 2 "$GET" com.lab126.powerd state 2>/dev/null)" = active ] || fail '请唤醒 Kindle 后启用'
    [ ! -e /var/local/system/userpasswdenabled ] || fail '请先关闭设备密码锁'
    [ -x "$APP/bin/kindle-rss-home" ] && [ -x "$APP/bin/kindle-rss" ] && [ -f /mnt/us/documents/RSS阅读器.sh ] || fail '请先完整安装 RSS 阅读器'
    [ ! -L /var/local/native-rss/session.lock ] || fail '无效阅读器会话锁'
    exec 9>/var/local/native-rss/session.lock; flock -n 9 || fail '请先退出 RSS 阅读器'
    flock -u 9; exec 9>&-
    if mounted && [ -f "$STATE/enabled" ] && owned_job; then
        if bridge_ready; then printf '%s\n' '首页 RSS 已启用'; return; fi
        disable_home
    fi
    prepare_copy
    [ ! -L "$JOB" ] || fail '启动配置不能是符号链接'
    [ ! -e "$JOB" ] || owned_job || fail '已有同名启动配置，拒绝覆盖'
    [ -f "$APP/home/native-rss-home.conf" ] && [ ! -L "$APP/home/native-rss-home.conf" ] || fail '缺少启动配置模板'
    pending=$(mktemp "$STATE/activate.XXXXXX") || fail '无法创建启用恢复标记'
    # Arm recovery before the first persistent change, not after restarting the UI.
    nohup sh "$SELF" rollback-after "$pending" 7>&- 8>&- 9>&- </dev/null >"$STATE/activation.log" 2>&1 &
    root_rw
    [ ! -e "$JOB.rss-tmp" ] && [ ! -L "$JOB.rss-tmp" ] || fail '临时启动配置已存在'
    : >"$STATE/installing-job" || fail '无法保存启动配置恢复标记'
    cp "$APP/home/native-rss-home.conf" "$JOB.rss-tmp" && chmod 644 "$JOB.rss-tmp" && mv "$JOB.rss-tmp" "$JOB" || fail '无法安装启动配置'
    rm -f "$STATE/installing-job"
    timeout 5 "$INIT" reload-configuration || fail '无法重载启动配置'
    root_restore || fail '无法恢复根分区只读状态'
    : >"$STATE/enabled"
    rm -f "$STATE/health"
    reload_home || fail '首页加载失败，正在恢复原首页'
    n=0
    while [ "$n" -lt 60 ]; do
        if bridge_ready; then
            rm -f "$pending"
            archive_report "$previous"
            printf '%s\n' '首页 RSS 已启用'
            return 0
        fi
        sleep 1; n=$((n+1))
    done
    fail '未收到首页数据确认，正在恢复原首页'
}

[ "$#" -ge 1 ] || fail '用法：home.sh enable|disable|status'
case "$1" in
    archive-after)
        [ "$#" -eq 2 ] || exit 1
        case "$2" in ''|*[!0-9]*) exit 1 ;; esac
        sleep 90
        archive_report "$2" quiet
        exit ;;
    rollback-after)
        [ "$#" -eq 2 ] || exit 1
        case "$2" in "$STATE"/activate.*) ;; *) exit 1 ;; esac
        sleep 120
        if [ -f "$2" ] && [ ! -L "$2" ]; then sh "$SELF" disable && rm -f "$2"; fi
        exit ;;
    status)
        if mounted; then printf '%s\n' '首页 RSS：已加载'; else printf '%s\n' '首页 RSS：未加载'; fi
        timeout 3 "$INIT" status native-rss-home 2>/dev/null || :
        exit ;;
    mount) mount_home; exit ;;
    unmount) unmount_home; exit ;;
    enable|disable) [ "$#" -eq 1 ] || fail '不接受额外参数' ;;
    *) fail '不支持的操作' ;;
esac
for tool in timeout flock stat sha256sum awk mktemp mount umount df; do command -v "$tool" >/dev/null 2>&1 || fail "缺少工具 $tool"; done
private_state
[ ! -L "$STATE/control.lock" ] || fail '无效控制锁'
exec 8>"$STATE/control.lock"; flock -n 8 || fail '启停操作正在进行'
finish() {
    result=$?
    trap - 0
    if [ "$result" -ne 0 ] && [ -n "${pending:-}" ] && [ -f "$pending" ]; then
        if ( disable_home ); then rm -f "$pending"; else result=1; fi
    fi
    root_restore || result=1
    exit "$result"
}
trap finish 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
if [ "$1" = enable ]; then enable_home; else disable_home; fi
