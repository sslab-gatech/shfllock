#!/bin/bash

locks=(table_spinlock table_aqs table_cna table_mutex table_aqm_mutex_fp \
       table_aqm_mutex_lnuma table_aqm_mutex_rnuma table_aqm_mutex_nfp \
       table_rwlock table_rwsem table_cmcsmcs)

cores=(1 2 4 12 24 48 96 120 144 168 192)

rw_writes=1
rw_total=100
buckets=1024
entries=4096

DIR=results-spinlock

for l in ${locks[@]}
do
	mkdir -p ${DIR}/$l
	for c in ${cores[@]}
	do
		sudo dmesg -C
		sudo insmod build/ht.ko reader_type=$l writer_type=$l \
			ro=0 rw=$c \
			rw_writes=${rw_writes} rw_total=${rw_total} \
			buckets=${buckets} \
			entries=${entries}
		sleep 10
		sudo rmmod ht
		sleep 1
		sudo dmesg > ${DIR}/$l/core.${c}
	done
done
