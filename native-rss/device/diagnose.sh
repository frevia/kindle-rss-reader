#!/bin/sh
# Read-only device inspection. Only the report file is written.
umask 077
ROOT=/mnt/us/extensions/native-rss-diagnostics
APP=$ROOT/kindle-rss
if [ -x /mnt/us/native-rss/bin/kindle-rss ]; then APP=/mnt/us/native-rss/bin/kindle-rss; fi
REPORT=/mnt/us/documents/RSS-device-report.txt
{
    echo 'Kindle RSS read-only diagnostics v4'
    date -u
    uname -a
    if [ -r /etc/prettyversion.txt ]; then cat /etc/prettyversion.txt; fi
    if [ -d /etc/upstart ]; then echo 'init_system=upstart'; else echo 'init_system=sysv'; fi
    if [ -f /etc/upstart/statusbar.conf ]; then echo 'independent_statusbar=present'; else echo 'independent_statusbar=absent'; fi
    if [ -e /lib/ld-linux-armhf.so.3 ]; then echo 'system_abi=armhf'; else echo 'system_abi=armel'; fi
    echo 'app_abi=static-armv7-softfloat'
    for tool in timeout flock stat pidof lipc-get-prop lipc-set-prop initctl; do
        printf 'tool_%s=' "$tool"
        command -v "$tool" || :
    done
    if timeout 2 /bin/true 2>/dev/null; then
        echo 'timeout_seconds_syntax=available'
        timeout 8 "$APP" probe
        printf 'probe_exit=%s\n' "$?"
        for job in lab126_gui statusbar; do
            timeout 2 initctl status "$job" 2>&1 || :
        done
        printf 'power_state='
        timeout 2 lipc-get-prop com.lab126.powerd state 2>&1 || :
        for property in battLevel isCharging; do
            printf 'powerd_%s=' "$property"
            timeout 2 lipc-get-prop -i com.lab126.powerd "$property" 2>&1 || :
        done
    else
        echo 'timeout_seconds_syntax=unavailable; native probe skipped'
    fi
    for name in awesome JunoStatusBarDriver volumd; do
        pids=$(pidof "$name" 2>/dev/null) || pids=
        printf 'process_%s=%s\n' "$name" "$pids"
        for pid in $pids; do
            case "$pid" in ''|*[!0-9]*) continue ;; esac
            if [ -r "/proc/$pid/comm" ]; then cat "/proc/$pid/comm"; fi
            if [ -r "/proc/$pid/status" ]; then awk '/^State:/{print}' "/proc/$pid/status"; fi
        done
    done
    if [ -x /mnt/us/libkh/bin/fbink ]; then
        echo 'fbink=present'
        timeout 5 /mnt/us/libkh/bin/fbink -q -e 2>&1
        printf 'fbink_probe_exit=%s\n' "$?"
    else echo 'fbink=missing'; fi
    for supply in max77696-battery max77796-battery max77796-charger bd7181x_bat bd71827_bat; do
        for field in capacity status charging; do
            file=/sys/class/power_supply/$supply/$field
            if [ -r "$file" ]; then printf 'power_supply_%s_%s=' "$supply" "$field"; head -c 128 "$file"; printf '\n'; fi
        done
    done
    if [ -f /var/local/system/userpasswdenabled ]; then echo 'passcode=enabled'; else echo 'passcode=not-detected'; fi
    for log in /var/local/native-rss/session.previous.log /var/local/native-rss/session.log; do
        if [ -f "$log" ] && [ ! -L "$log" ]; then
            printf '\n--- %s (last 64 lines) ---\n' "$log"
            tail -n 64 "$log"
        fi
    done
    echo 'No framebuffer write, input grab, process suspension, or firmware modification was requested.'
} >"$REPORT" 2>&1
sync
