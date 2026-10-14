#!/bin/sh
# Probe a Shield TV over adb. Usage: sh tools/shield-probe.sh [adb-args...]
# ponytail: read-only, one file, no deps beyond adb.
set -u
A="${*:-}"
echo "== build =="
adb $A shell 'getprop ro.product.model; getprop ro.product.device; getprop ro.board.platform; getprop ro.hardware; getprop ro.build.version.release; getprop ro.build.version.sdk; uname -r' 2>&1
echo "== gpu blobs =="
adb $A shell 'ls /vendor/lib64/hw/ 2>/dev/null | grep -i -E "vulkan|egl|gl"; ls /dev | grep -E "nvhost|nvmap|kgsl|dri" ; ls /dev/dri 2>&1' 2>&1
echo "== sysfs =="
adb $A shell 'ls /sys/class/kgsl 2>&1; ls /sys/devices/soc0 2>&1 | head -5; cat /sys/devices/soc0/machine 2>&1' 2>&1
echo "== vulkan feature =="
adb $A shell 'cmd package list features 2>/dev/null | grep -i vulkan; dumpsys package com.droiddeck.launcher 2>/dev/null | head -3' 2>&1
echo "== cpu/abi =="
adb $A shell 'getprop ro.product.cpu.abi; cat /proc/cpuinfo | grep -m4 "model name\|Hardware\|Processor"' 2>&1
