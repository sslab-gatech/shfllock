#!/bin/bash

set -x

# $1 --> iso input to be used, fedora-core image provided in the dataset
INPUT_ISO=$1

LOCK_DIR=./../../ulocks/src/litl

LOCKS=(libmcs_spinlock.sh libhmcs_original.sh \
       libaqswonode_spinlock.sh libcbomcs_spinlock.sh \
       libcna_spinlock.sh libmalthusian_spinlock.sh \
       libpthreadinterpose_original.sh libmutexee_original.sh \
       libmcstp_original.sh libaqmwonode_spin_then_park.sh)


DIR=streamcluster-results

for i in `seq 1 1`
do
	for l in ${LOCKS[@]}
	do
		mkdir -p ${DIR}/$i/$l
		for c in 1 2 4 12 `seq 24 24 192`
		do
			/usr/bin/time ${LOCK_DIR}/${l} \
				taskset -c 0-$(($c-1)) \
				./pkgs/kernels/streamcluster/inst/amd64-linux.gcc/bin/streamcluster \
				10 20 32 4096 4096 1000 none output.txt $c  2>> ${DIR}/$i/$l/core.${c} >> ${DIR}/$i/$l/core.${c}
		done
	done
done


LOCKS=(libpthreadinterpose_original.sh libhmcs_original.sh \
       libmcs_spinlock.sh libcna_spinlock.sh \
       libaqswonode_spinlock.sh libaqmwonode_spin_then_park.sh)

DIR=dedup-results

for i in `seq 1 1`
do
	for l in ${LOCKS[@]}
	do
		mkdir -p ${DIR}/$i/$l
		for c in 1 2 4 12 `seq 24 24 192`
		do
			/usr/bin/time ${LOCK_DIR}/${l} \
				./pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup \
				-c -p -v \
				-i ${INPUT_ISO} \
				-o /dev/null -t $c 2>> ${DIR}/$i/$l/core.${c} >> ${DIR}/$i/$l/core.${c}
		done
	done
done
