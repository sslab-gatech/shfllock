#!/bin/bash

sudo sh -c "echo 0 > /proc/sys/kernel/kptr_restrict"
sudo sh -c "echo core > /proc/sys/kernel/core_pattern"

sudo cd /sys/devices/system/cpu
sudo sh -c "echo performance | tee cpu*/cpufreq/scaling_governor"

sudo rm -f /dev/shm/*.synced_queue

# create input and output directory for fuzzing
odir=/tmp/mosbench/tmpfs-separate/1/output
idir=/tmp/mosbench/tmpfs-separate/0

cp -r input $idir/

if [[ -d "$odir" ]]
then
    rm -fr $odir
fi

mkdir -p $odir
