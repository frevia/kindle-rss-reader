#!/bin/sh
# Read-only device inspection. Only the report file is written.
umask 077
ROOT=/mnt/us/extensions/native-rss-diagnostics
APP=$ROOT/kindle-rss
if [ -x /mnt/us/native-rss/bin/kindle-rss ]; then APP=/mnt/us/native-rss/bin/kindle-rss; fi
REPORT=/mnt/us/documents/RSS-device-report.txt
{
    echo 'Kindle RSS read-only diagnostics v1'
    date -u
    uname -a
    if [ -r /etc/prettyversion.txt ]; then cat /etc/prettyversion.txt; fi
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
    if [ -x /mnt/us/libkh/bin/fbink ]; then echo 'fbink=present'; else echo 'fbink=missing'; fi
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
