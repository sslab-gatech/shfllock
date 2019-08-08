#!/bin/bash

set -x

# locks=(orig_qspinlock cmcsmcs aqs aqm)
locks=(mutex aqs aqm_fp aqm_nfp aqm_lnuma aqm_rnuma)
cores=(1 2 4 8 10 20 30 40 50 60 70 80 120 160)
clines=(0 1 2 4 8 16)
delay=(0 1 10 100 1000)

for l in ${locks[@]}
do
        mkdir -p $l
        for c in 1 2 4 8 `seq 10 10 80`
        do
                for cline in ${clines[@]}
                do
                        for d in ${delay[@]}
                        do
                                sudo dmesg -C
                                sudo insmod build/stress.ko threads=$c stress_type="$l" dirty_clines=${cline} delay=$d
                                sleep 10
                                sudo rmmod stress
                                sleep 2
                                sudo dmesg > $l/cores.$c.clines.${cline}.delay.$d.out
                                sync
                        done
                done
        done
done
