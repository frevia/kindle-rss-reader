#!/bin/sh
# Development launcher for the verified KPW3 layout; no device installation is implicit.
umask 077
STATE=/var/local/native-rss
APP=/mnt/us/native-rss/bin/kindle-rss
DB=/mnt/us/native-rss/data/rss.sqlite
FB=/dev/fb0
PROC=/proc
GET=/usr/bin/lipc-get-prop
SET=/usr/bin/lipc-set-prop
REFRESH=/mnt/us/libkh/bin/fbink
KILL=kill
PASSCODE=/var/local/system/userpasswdenabled

fail() { printf '%s\n' "Kindle RSS: $*" >&2; exit 1; }
[ "$#" -eq 0 ] || fail '不接受启动参数'
[ ! -e "$PASSCODE" ] || fail '设备启用了密码锁，不能接管系统界面'
for tool in timeout flock stat pidof awk dd mktemp mkfifo; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少工具 $tool"
done
for tool in "$APP" "$GET" "$SET" "$REFRESH"; do
    [ -x "$tool" ] || fail "缺少程序 $tool"
done
[ ! -L "$STATE" ] || fail '状态目录不能是符号链接'
mkdir -p "$STATE" || fail '无法创建状态目录'
# /var/local on Kindle inherits setgid; it does not grant group access to a 2700 directory.
case "$(stat -c '%a:%u' "$STATE")" in 700:0|2700:0) ;; *) fail '状态目录必须属于 root 且仅 root 可访问' ;; esac
[ ! -L "$STATE/session.lock" ] || fail '无效锁文件'
exec 9>"$STATE/session.lock" || fail '无法打开锁'
flock -n 9 || fail '阅读器已经运行'
for log in "$STATE/session.log" "$STATE/session.previous.log"; do
    [ ! -L "$log" ] || fail '日志不能是符号链接'
done
if [ -e "$STATE/session.log" ]; then
    mv -f "$STATE/session.log" "$STATE/session.previous.log" || fail '无法保留上次运行日志'
fi
exec >"$STATE/session.log" 2>&1 || fail '无法创建运行日志'
printf 'session_start '; date -u
snapshot=
CRE_CACHE=
stopped=
pillow=0
child=
launching=0
wm_pid=
wm_born=
guardian=
recovering=0
owner=$$
owner_born=$(awk '{print $22}' "/proc/$owner/stat") || fail '无法确认启动器身份'
case "$owner_born" in ''|*[!0-9]*) fail '无效启动器身份' ;; esac
done_file=
live_fifo=

owner_alive() {
    [ "$(awk '$3 != "Z" {print $22}' "/proc/$owner/stat" 2>/dev/null)" = "$owner_born" ]
}

awake() { [ "$(timeout 2 "$GET" com.lab126.powerd state 2>/dev/null)" = active ]; }
identity() {
    [ -r "$PROC/$1/comm" ] && [ -r "$PROC/$1/stat" ] || return 1
    IFS= read -r name <"$PROC/$1/comm" || return 1
    [ "$name" = "$2" ] || return 1
    awk '{print $22}' "$PROC/$1/stat"
}
same() { [ "$(identity "$1" "$2")" = "$3" ]; }
stop_child() {
    [ -n "$child" ] || return
    "$KILL" -TERM "$child" 2>/dev/null || :
    n=0
    while "$KILL" -0 "$child" 2>/dev/null && [ "$n" -lt 3 ]; do sleep 1; n=$((n+1)); done
    "$KILL" -KILL "$child" 2>/dev/null || :
    wait "$child" 2>/dev/null || :
    child=
}
cleanup() {
    result=$?
    printf 'session_cleanup recovering=%s exit=%s\n' "$recovering" "$result"
    trap - 0 HUP INT TERM
    stop_child
    restore_failed=0
    if [ -n "$CRE_CACHE" ]; then rm -rf "$CRE_CACHE" || restore_failed=1; fi
    # Never overwrite a screensaver after powerd has taken the screen.
    if [ -n "$snapshot" ] && [ -n "$stopped" ] && same "$wm_pid" awesome "$wm_born" && awake; then
        dd if="$snapshot" of="$FB" bs=1088 count=1448 conv=notrunc 2>/dev/null || restore_failed=1
    fi
    for record in $stopped; do
        old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
        [ "$2" != volumd ] || continue
        if same "$1" "$2" "$3"; then "$KILL" -CONT "$1" 2>/dev/null || restore_failed=1; fi
    done
    if [ "$pillow" -eq 1 ]; then timeout 2 "$SET" com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || restore_failed=1; fi
    if [ -n "$stopped" ] && same "$wm_pid" awesome "$wm_born" && awake; then
        timeout 3 "$SET" com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || restore_failed=1
        if awake; then timeout 5 "$REFRESH" -q -s >/dev/null 2>&1 || restore_failed=1; fi
    fi
    for record in $stopped; do
        old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
        if [ "$2" = volumd ] && same "$1" "$2" "$3"; then "$KILL" -CONT "$1" 2>/dev/null || restore_failed=1; fi
    done
    if [ -n "$guardian" ] && [ "$recovering" -eq 0 ]; then
        : >"$done_file" || "$KILL" -TERM "$guardian" 2>/dev/null
        wait "$guardian" 2>/dev/null || :
    fi
    [ -z "$done_file" ] || rm -f "$done_file"
    [ -z "$live_fifo" ] || rm -f "$live_fifo"
    [ -z "$snapshot" ] || rm -f "$snapshot"
    if [ "$restore_failed" -ne 0 ]; then printf '%s\n' 'Kindle RSS: 系统界面恢复不完整，请查看日志' >&2; result=1; fi
    printf 'session_end exit=%s restore_failed=%s\n' "$result" "$restore_failed"
    exit "$result"
}
interrupted() {
    if [ -z "$child" ] && [ "$launching" -eq 1 ]; then child=$!; fi
    exit "$1"
}
trap cleanup 0
trap 'interrupted 129' HUP
trap 'interrupted 130' INT
trap 'interrupted 143' TERM

awake || fail '请在 Kindle 唤醒时启动'
probe=$(timeout 5 "$APP" probe) || fail '设备预检失败'
for value in width=1072 height=1448 stride=1088 bpp=8 xoffset=0 yoffset=0 rotation=3 driver=mxc_epdc_fb layout=accepted; do
    printf '%s\n' "$probe" | grep -qx "$value" || fail "屏幕参数不符：$value"
done
records=
for service in awesome JunoStatusBarDriver volumd; do
    pids=$(pidof "$service") || fail "未找到 $service"
    set -- $pids
    [ "$#" -eq 1 ] || fail "$service 进程数量异常"
    case "$1" in ''|*[!0-9]*) fail '无效进程号' ;; esac
    comm=$service
    [ "$service" != JunoStatusBarDriver ] || comm=JunoStatusBarDr
    born=$(identity "$1" "$comm") || fail "无法确认 $service 身份"
    case "$born" in ''|*[!0-9]*) fail '无效进程启动时间' ;; esac
    state=$(awk '/^State:/{print $2}' "$PROC/$1/status")
    case "$state" in R|S|I) ;; *) fail "$service 当前状态不适合接管" ;; esac
    if [ "$service" = awesome ]; then wm_pid=$1; wm_born=$born; fi
    records="$records $1:$comm:$born"
done
snapshot=$(mktemp "$STATE/frame.XXXXXX") || fail '无法保存屏幕'
dd if="$FB" of="$snapshot" bs=1088 count=1448 2>/dev/null || fail '保存屏幕失败'
[ "$(wc -c <"$snapshot")" -eq 1575424 ] || fail '屏幕快照不完整'
awake || fail '设备已休眠'
CRE_CACHE=$(mktemp -d /tmp/rss-cre-session.XXXXXX) || fail '无法创建新版排版缓存'
export RSS_CRE_CACHE=$CRE_CACHE
done_file=$snapshot.done
live_fifo=$snapshot.live
mkfifo -m 600 "$live_fifo" || fail '无法创建会话生命周期通道'
exec 8<>"$live_fifo" || fail '无法打开会话生命周期通道'
exec 7<"$live_fifo" || fail '无法监听会话生命周期通道'
# This independent shell retains the session lock and recovery state if the launcher dies.
(
    trap - 0 HUP INT TERM
    exec 8>&-
    recovering=1
    stopped=$records
    pillow=1
    while owner_alive; do
        [ ! -e "$done_file" ] || exit 0
        sleep 1
    done
    # SIGKILL may still be pending: wait for the reader to actually close its inherited fd.
    cat <&7 >/dev/null
    cleanup
) &
guardian=$!
exec 7<&-
pillow=1
timeout 2 "$SET" com.lab126.pillow disableEnablePillow disable || fail '无法关闭系统浮层'
for record in $records; do
    old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
    same "$1" "$2" "$3" || fail '系统进程已改变'
    stopped="$record $stopped"
    "$KILL" -STOP "$1" || fail '无法暂停系统界面'
done
# Holding volumd prevents USB mass-storage from exporting the live database volume.
launching=1
RSS_UI_GUARD=1 RSS_LAUNCHER_PID=$owner "$APP" --db "$DB" device 9>&- &
child=$!
launching=0
printf 'reader_started pid=%s\n' "$child"
while "$KILL" -0 "$child" 2>/dev/null; do
    "$KILL" -0 "$guardian" 2>/dev/null || fail '恢复监护进程已退出'
    [ "$(awk '/^State:/{print $2}' "/proc/$guardian/status" 2>/dev/null)" != Z ] || fail '恢复监护进程已退出'
    if ! awake; then stop_child; exit 0; fi
    sleep 1
done
wait "$child"; result=$?
child=
exit "$result"
