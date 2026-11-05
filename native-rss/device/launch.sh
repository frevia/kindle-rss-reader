#!/bin/sh
# Capability-checked portrait session; no device installation is implicit.
umask 077
STATE=/var/local/native-rss
APP=/mnt/us/native-rss/bin/kindle-rss
DB=/mnt/us/native-rss/data/rss.sqlite
FB=/dev/fb0
PROC=/proc
GET=/usr/bin/lipc-get-prop
SET=/usr/bin/lipc-set-prop
REFRESH=/mnt/us/libkh/bin/fbink
FBINK_ALTERNATES="/usr/bin/fbink /mnt/us/koreader/fbink"
KILL=kill
PASSCODE=/var/local/system/userpasswdenabled
UPDATE=/mnt/us/native-rss/.update
LAUNCH=/mnt/us/native-rss/launch.sh
INITCTL=/sbin/initctl
STATUSBAR_JOB=/etc/upstart/statusbar.conf
UPSTART_DIR=/etc/upstart
FIRMWARE=/etc/prettyversion.txt

fail() { printf '%s\n' "Kindle RSS: $*" >&2; exit 1; }
article=
if [ "$#" -ne 0 ]; then
    [ "$#" -eq 2 ] && [ "$1" = --article ] || fail '启动参数仅支持 --article ID'
    article=$2
    case "$article" in ''|0*|*[!0-9]*) fail '无效文章编号' ;; esac
    [ "${#article}" -le 16 ] && [ "$article" -le 9007199254740991 ] || fail '无效文章编号'
fi
[ ! -e "$PASSCODE" ] || fail '设备启用了密码锁，不能接管系统界面'
for tool in timeout flock stat pidof awk dd mktemp mkfifo head; do
    command -v "$tool" >/dev/null 2>&1 || fail "缺少工具 $tool"
done
for tool in "$GET" "$SET"; do
    [ -x "$tool" ] || fail "缺少程序 $tool"
done
[ ! -L "$STATE" ] || fail '状态目录不能是符号链接'
mkdir -p "$STATE" || fail '无法创建状态目录'
# Framework startup applies g=u and setgid recursively under /var/local.
case "$(stat -c '%a:%u' "$STATE")" in 700:0|2700:0|2770:0) ;; *) fail '状态目录必须属于 root 且仅 root 可访问' ;; esac
chmod 0700 "$STATE" || fail '无法恢复状态目录的私有权限'
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
# Recovery runs under the same session lock, before opening the database or taking the screen.
if [ -e "$UPDATE/journal" ] && [ "${RSS_UPDATE_BOOT:-0}" != 1 ]; then
    [ -x "$UPDATE/runner" ] && [ ! -L "$UPDATE/runner" ] || fail '升级恢复程序缺失，请通过 USB 恢复'
    RSS_UPDATE_GUARD=1 exec "$UPDATE/runner" update-recover
    fail '无法运行升级恢复程序'
fi
[ -x "$APP" ] || fail "缺少程序 $APP"
install_update=0
open_browser=0
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
statusbar_expected=
statusbar_name=
statusbar_born=
pillow_mode=

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
record_process() {
    pid=$1; comm=$2
    case "$pid" in ''|*[!0-9]*) fail '无效进程号' ;; esac
    case "$comm" in ''|*[!a-zA-Z0-9_.-]*) fail '无效进程名称' ;; esac
    born=$(identity "$pid" "$comm") || fail "无法确认 $comm 身份"
    case "$born" in ''|*[!0-9]*) fail '无效进程启动时间' ;; esac
    process_state=$(awk '/^State:/{print $2}' "$PROC/$pid/status")
    case "$process_state" in R|S|I) ;; *) fail "$comm 当前状态不适合接管" ;; esac
    case " $records " in *" $pid:"*) return ;; esac
    records="$records $pid:$comm:$born"
}
statusbar_pid() {
    job_status=$(LC_ALL=C timeout 2 "$INITCTL" status statusbar) || return 1
    case "$job_status" in
        'statusbar stop/waiting') printf '0\n' ;;
        'statusbar start/running, process '*)
            job_pid=${job_status#'statusbar start/running, process '}
            case "$job_pid" in ''|0*|*[!0-9]*) return 1 ;; esac
            [ "${#job_pid}" -le 10 ] && [ "$job_pid" -gt 1 ] && [ "$job_pid" -le 2147483647 ] || return 1
            printf '%s\n' "$job_pid"
            ;;
        *) return 1 ;;
    esac
}
statusbar_unchanged() {
    [ -n "$statusbar_expected" ] || return 0
    [ "$(statusbar_pid)" = "$statusbar_expected" ] || return 1
    [ "$statusbar_expected" = 0 ] || same "$statusbar_expected" "$statusbar_name" "$statusbar_born"
}
firmware_pillow_mode() {
    [ -d "$UPSTART_DIR" ] || return 1
    [ -r "$FIRMWARE" ] || return 1
    firmware_text=$(head -c 1025 "$FIRMWARE") || return 1
    [ "${#firmware_text}" -le 1024 ] || return 1
    printf '%s\n' "$firmware_text" | awk '
        $1 == "Kindle" {
            count++; n=split($2, v, ".")
            if(n<2 || n>5 || v[1]!="5") bad=1
            for(i=1;i<=n;i++) if(v[i]!~/^[0-9]+$/ || length(v[i])>3 || (length(v[i])>1 && substr(v[i],1,1)=="0")) bad=1
            mode=(v[2]+0<6 || (v[2]+0==6 && v[3]+0<5))?"soft":"hard"
        }
        END {if(count!=1 || bad)exit 1; print mode}'
}
pillow_change() {
    case "$pillow_mode:$1" in
        hard:disable|hard:enable) timeout 2 "$SET" com.lab126.pillow disableEnablePillow "$1" ;;
        soft:disable) timeout 2 "$SET" com.lab126.pillow interrogatePillow '{"pillowId": "default_status_bar", "function": "nativeBridge.hideMe();"}' ;;
        soft:enable) timeout 2 "$SET" com.lab126.pillow interrogatePillow '{"pillowId": "default_status_bar", "function": "nativeBridge.showMe();"}' ;;
        *) return 1 ;;
    esac
}
probe_value() {
    printf '%s\n' "$probe" | awk -F= -v key="$1" '$1 == key {value=$2; count++} END {if(count != 1) exit 1; print value}'
}
probe_number() {
    number=$(probe_value "$1") || return 1
    case "$number" in ''|*[!0-9]*|0[0-9]*) return 1 ;; esac
    [ "${#number}" -le 8 ] || return 1
    printf '%s\n' "$number"
}
fbink_value() {
    # FBInk emits shell assignments; parse only the numeric fields, never eval them.
    printf '%s\n' "$fbink_probe" | awk -v key="$1" 'BEGIN {RS=";"; FS="="} $1 == key {value=$2; count++} END {if(count != 1) exit 1; print value}'
}
fbink_compatible() {
    [ -x "$1" ] || return 1
    fbink_probe=$(timeout 5 "$1" -q -e) || return 1
    fbink_id=$(fbink_value deviceId) || return 1
    case "$fbink_id" in ''|0*|*[!0-9]*) return 1 ;; esac
    [ "${#fbink_id}" -le 5 ] && [ "$fbink_id" -le 65535 ] || return 1
    [ "$(fbink_value isSunxi)" = 0 ] && [ "$(fbink_value invertedGrayscale)" = 0 ] &&
        [ "$(fbink_value BPP)" = 8 ] && [ "$(fbink_value screenWidth)" = "$width" ] &&
        [ "$(fbink_value screenHeight)" = "$height" ] && [ "$(fbink_value lineLength)" = "$stride" ]
}
screen_signature() {
    printf '%s\n' "$1" | awk -F= '$1 ~ /^(width|height|stride|bpp|xoffset|yoffset|virtual_height|memory_size|rotation|grayscale|driver)$/ {print}'
}
screen_unchanged() {
    current_probe=$(timeout 5 "$APP" probe) || return 1
    [ "$(screen_signature "$current_probe")" = "$original_screen" ]
}
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
        if screen_unchanged; then
            dd if="$snapshot" of="$FB" bs="$stride" count="$snapshot_rows" conv=notrunc 2>/dev/null || restore_failed=1
        else
            printf '%s\n' 'screen_restore=skipped; framebuffer layout changed or unavailable'
        fi
    fi
    for record in $stopped; do
        old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
        case "$2" in volumd) continue ;; esac
        if same "$1" "$2" "$3"; then "$KILL" -CONT "$1" 2>/dev/null || restore_failed=1; fi
    done
    if [ "$pillow" -eq 1 ]; then pillow_change enable >/dev/null 2>&1 || restore_failed=1; fi
    if [ -n "$stopped" ] && same "$wm_pid" awesome "$wm_born" && awake; then
        timeout 3 "$SET" com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || restore_failed=1
        if awake; then timeout 5 "$REFRESH" -q -s >/dev/null 2>&1 || restore_failed=1; fi
    fi
    for record in $stopped; do
        old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
        case "$2" in
            volumd) if same "$1" "$2" "$3"; then "$KILL" -CONT "$1" 2>/dev/null || restore_failed=1; fi ;;
        esac
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
    if [ "$recovering" -eq 0 ] && [ "$restore_failed" -eq 0 ] && [ "$install_update" -eq 2 ]; then
        # BusyBox timeout watchdogs may still hold an inherited copy of this lock.
        flock -u 9 || fail '无法释放升级会话锁，请重新打开阅读器'
        RSS_UPDATE_BOOT=1 exec /bin/sh "$LAUNCH" 8>&- 9>&-
    fi
    if [ "$recovering" -eq 0 ] && [ "$restore_failed" -eq 0 ] && [ "$result" -eq 0 ] && [ "$open_browser" -eq 1 ] && awake; then
        RSS_BROWSER_GUARD=1 timeout 15 "$APP" --db "$DB" open-directory-website 8>&- || result=1
    fi
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
pillow_mode=$(firmware_pillow_mode) || fail '无法确认 Upstart／Kindle 5 固件的浮层策略，请导出设备诊断报告'
printf 'pillow_mode=%s\n' "$pillow_mode"
probe=$(timeout 5 "$APP" probe) || fail '设备预检失败'
[ "$(probe_value layout)" = accepted ] || fail '不支持的屏幕布局，请导出设备诊断报告'
[ "$(probe_value input)" = accepted ] || fail '未找到唯一受支持的触摸屏，请导出设备诊断报告'
width=$(probe_number width) && height=$(probe_number height) && stride=$(probe_number stride) &&
    xoffset=$(probe_number xoffset) && yoffset=$(probe_number yoffset) &&
    snapshot_bytes=$(probe_number snapshot_bytes) || fail '无效屏幕参数'
[ "$width" -ge 600 ] && [ "$width" -le 2560 ] && [ "$height" -ge 800 ] && [ "$height" -le 3400 ] &&
    [ "$width" -le "$height" ] && [ "$stride" -ge "$width" ] && [ "$stride" -le 8192 ] &&
    [ "$xoffset" -le "$((stride-width))" ] && [ "$yoffset" -le 65536 ] &&
    [ "$(probe_value bpp)" = 8 ] || fail '屏幕参数超出兼容范围'
snapshot_rows=$((yoffset+height))
[ "$snapshot_bytes" -eq "$((stride*snapshot_rows))" ] && [ "$snapshot_bytes" -le 67108864 ] || fail '无效屏幕快照长度'
original_screen=$(screen_signature "$probe")
printf '%s\n' "$probe"
fbink_selected=
ulimit -c 0
for candidate in "$REFRESH" $FBINK_ALTERNATES; do
    if fbink_compatible "$candidate"; then fbink_selected=$candidate; break; fi
done
[ -n "$fbink_selected" ] || fail '没有通过设备预检的 FBInk，请导出设备诊断报告'
REFRESH=$fbink_selected
RSS_FBINK=$REFRESH; export RSS_FBINK
printf 'fbink_program=%s\n' "$REFRESH"
records=
for service in awesome JunoStatusBarDriver volumd; do
    if ! pids=$(pidof "$service"); then
        [ "$service" != JunoStatusBarDriver ] || continue
        fail "未找到 $service"
    fi
    set -- $pids
    [ "$#" -eq 1 ] || fail "$service 进程数量异常"
    case "$1" in ''|*[!0-9]*) fail '无效进程号' ;; esac
    comm=$service
    [ "$service" != JunoStatusBarDriver ] || comm=JunoStatusBarDr
    record_process "$1" "$comm"
    if [ "$service" = awesome ]; then wm_pid=$1; wm_born=$born; fi
done
if [ -f "$STATUSBAR_JOB" ]; then
    [ -x "$INITCTL" ] || fail '缺少独立状态栏查询工具'
    statusbar_expected=$(statusbar_pid) || fail '无法确认独立状态栏状态'
    if [ "$statusbar_expected" != 0 ]; then
        IFS= read -r statusbar_name <"$PROC/$statusbar_expected/comm" || fail '无法读取独立状态栏进程身份'
        record_process "$statusbar_expected" "$statusbar_name"
        statusbar_born=$born
    fi
    printf 'statusbar_pid=%s\n' "$statusbar_expected"
fi
# The home bridge keeps heartbeats alive; session.lock suppresses content updates.
screen_unchanged || fail '屏幕模式在预检后改变'
snapshot=$(mktemp "$STATE/frame.XXXXXX") || fail '无法保存屏幕'
dd if="$FB" of="$snapshot" bs="$stride" count="$snapshot_rows" 2>/dev/null || fail '保存屏幕失败'
[ "$(wc -c <"$snapshot")" -eq "$snapshot_bytes" ] || fail '屏幕快照不完整'
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
statusbar_unchanged || fail '独立状态栏进程在接管前改变'
pillow=1
pillow_change disable || fail '无法关闭系统浮层'
for record in $records; do
    old_ifs=$IFS; IFS=:; set -- $record; IFS=$old_ifs
    same "$1" "$2" "$3" || fail '系统进程已改变'
    if [ "$1" = "$statusbar_expected" ]; then statusbar_unchanged || fail '独立状态栏进程在接管前改变'; fi
    stopped="$record $stopped"
    "$KILL" -STOP "$1" || fail '无法暂停系统界面'
done
# Holding volumd prevents USB mass-storage from exporting the live database volume.
launching=1
if [ -n "$article" ]; then set -- --article "$article"; else set --; fi
RSS_UI_GUARD=1 RSS_LAUNCHER_PID=$owner "$APP" --db "$DB" "$@" device 9>&- &
child=$!
launching=0
printf 'reader_started pid=%s\n' "$child"
while "$KILL" -0 "$child" 2>/dev/null; do
    "$KILL" -0 "$guardian" 2>/dev/null || fail '恢复监护进程已退出'
    [ "$(awk '/^State:/{print $2}' "/proc/$guardian/status" 2>/dev/null)" != Z ] || fail '恢复监护进程已退出'
    if ! awake; then stop_child; exit 0; fi
    statusbar_unchanged || fail '独立状态栏进程已改变，停止阅读并恢复系统'
    sleep 1
done
wait "$child"; result=$?
child=
if [ "$result" -eq 43 ]; then open_browser=1; result=0; fi
if [ "$result" -eq 42 ]; then
    [ -x "$UPDATE/runner" ] && [ ! -L "$UPDATE/runner" ] || fail '升级安装程序缺失'
    # The old shell keeps its open script inode; replacements use rename, never in-place writes.
    # Keep volumd paused and the guardian alive until every installation write has finished.
    launching=1
    RSS_UPDATE_GUARD=1 RSS_LAUNCHER_PID=$owner "$UPDATE/runner" update-apply 9>&- &
    child=$!
    launching=0
    while "$KILL" -0 "$child" 2>/dev/null; do
        "$KILL" -0 "$guardian" 2>/dev/null || fail '升级期间恢复监护进程已退出'
        sleep 1
    done
    wait "$child"; result=$?
    child=
    if [ "$result" -eq 0 ]; then install_update=2; fi
fi
exit "$result"
