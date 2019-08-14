#!/bin/bash

set -x

for i in 12 `seq 24 24 192`
do
        ./afl-launch.py $i 300 >> out.data
done
