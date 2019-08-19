#!/bin/bash

# $1 --> number of threads
# $2 --> millisecond
#db_bench --benchmarks=readrandom --threads=$1 --num=10 --time_ms=$2 2>&1 | grep readrandom
set -x
LOCK_DIR=./../../ulocks/src/litl

LOCKS=(libmcs_spinlock.sh libhmcs_original.sh \
       libaqs_spinlock.sh libcbomcs_spinlock.sh \
       libcna_spinlock.sh libmalthusian_spinlock.sh \
       libpthreadinterpose_original.sh libaqm_spin_then_park.sh)

DIR=results

for l in ${LOCKS[@]}
do
	mkdir -p ${DIR}/$l
	for c in 1 2 4 8 `seq 24 24 192` 384
	do
		${LOCK_DIR}/${l} taskset -c 0-$(($c - 1)) ./out-static/db_bench --benchmarks=readrandom --num=20 --time_ms=60000 --threads=$c >> ${DIR}/${l}/core.${c}
	done
done
