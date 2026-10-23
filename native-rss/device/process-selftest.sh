#!/bin/sh
# Exercises only its own child processes; does not take over Kindle services or devices.
umask 077
{
    echo 'Kindle RSS process lifecycle self-test v3 (assertions enabled)'
    date -u
    if [ -x /bin/test ]; then echo 'bin_test=present'; else echo 'bin_test=absent'; fi
    timeout 15 /mnt/us/extensions/native-rss-diagnostics/process_test
    result=$?
    printf 'process_test_exit=%s\n' "$result"
    echo 'No framebuffer write, input grab, or system service suspension was requested.'
} >/mnt/us/documents/RSS-process-report.txt 2>&1
sync
