#!/usr/bin/env python3

import sys
from progressbar import *
from progress.bar import *


class ShowBar(FillingCirclesBar):
    suffix = '%(percent).0f%%  [%(index)d / %(total)d done]'

    @property
    def total(self):
        return total


total = 0
locks = {}
data = open(sys.argv[1], 'r').readlines()
total = len(data)
# bar = ShowBar('Reading data from file...', max=total)
cur_tid = -1
prev_rel_time = 0
print("cs-time,non-cs-time,lock-acq-time,lock-rel-time")
for index, v in enumerate(data):
    if '0x' in v:
        d = v.split(' ')
        addr = d[0]
        tid, bl, al, bul, aul = \
            int(d[1]), int(d[2]), int(d[3]), int(d[4]), int(d[5])
        if tid != 1:
            break
        cs_time = bul - al
        lock_acq_time = al - bl
        lock_rel_time = aul - bul
        if tid != cur_tid:
            cur_tid = tid
            prev_rel_time = bl
        non_cs_time = bl - prev_rel_time
        prev_rel_time = aul
        # if addr not in locks:
        #     locks[addr] = [(cs_time, non_cs_time, lock_acq_time, lock_rel_time)]
        # else:
        #     locks[addr].append((cs_time, non_cs_time,
                                # lock_acq_time, lock_rel_time))
        print("%s,%d,%d,%d" % (cs_time, non_cs_time, lock_acq_time,
                               lock_rel_time))
    # bar.next()
# bar.finish()
