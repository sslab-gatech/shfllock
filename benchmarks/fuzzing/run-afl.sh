#!/bin/bash

# $1 -> num processes
# $2 -> time

sudo ./mkmounts tmpfs-separate
sudo ./prepare.sh

input=/tmp/mosbench/tmpfs-separate/0/input
output=/tmp/mosbench/tmpfs-separate/1/output

if [[ -d $output ]]
then
        rm -rf $output/*
fi

sudo rm -f /dev/shm/*.synced_queue
v=$(($1 - 1))
echo $v
for i in `seq 0 $v`
do
        ./afl/afl-fuzz -i $input \
                -o $output -S fuzzer$i \
                -u $i/$1 \
                jpeg-9b/.libs/lt-djpeg > /dev/null &
        sleep 0.1
done

sleep $2
pkill -9 afl
sleep 1

TMP=`mktemp -t .afl-info-XXXXXXXX` || exit 1
total_execs=0
total_time=0

for i in `find $output -name fuzzer_stats`
do
        sed 's/^command_line.*$/_skip:1/;s/[ ]*:[ ]*/="/;s/$/"/' "$i" >"$TMP"
        . $TMP
        total_execs=$(($total_execs + $execs_done))
        total_time=$(($total_time + $last_update - $start_time))
done
tput=`echo "scale=3;($total_execs / $total_time)" | bc`
echo "$1   $tput"
