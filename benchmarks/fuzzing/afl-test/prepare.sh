#!/bin/sh

sudo sh -c "echo 0 > /proc/sys/kernel/kptr_restrict"
sudo sh -c "echo core > /proc/sys/kernel/core_pattern"

#sudo cd /sys/devices/system/cpu
#sudo sh -c "echo performance | tee cpu*/cpufreq/scaling_governor"

sudo rm -f /dev/shm/*.synced_queue

cp -r input /tmp/mosbench/tmpfs-separate/0/
mkdir /tmp/mosbench/tmpfs-separate/1/output
