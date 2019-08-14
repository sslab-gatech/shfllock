#!/usr/bin/env python3

import subprocess
import threading
import time
import shutil
import os
import sys
from datetime import datetime

NUM_CPUS = int(sys.argv[1])
EXEC_TIME = int(sys.argv[2])

INPUT_DIR = "/tmp/mosbench/tmpfs-separate/0/input"
OUTPUT_DIR = "/tmp/mosbench/tmpfs-separate/1/output"

stop_flag = False
execs = [0 for x in range(0, NUM_CPUS)]
secs = [0 for x in range(0, NUM_CPUS)]
stopped = [False for x in range(0, NUM_CPUS)]


def update_stats(cpu):
    global execs
    global secs
    global OUTPUT_DIR

    filename = os.path.join(OUTPUT_DIR, 'fuzzer%d' % cpu, 'fuzzer_stats')
    if not os.path.exists(filename):
        return

    data = open(filename, 'r').readlines()
    start_time = 0
    last_update = 0
    execs_done = 0
    for d in data:
        if 'start_time' in d:
            start_time = int(d.split(':')[1])
        if 'last_update' in d:
            last_update = int(d.split(':')[1])
        if 'execs_done' in d:
            execs_done = int(d.split(':')[1])
    if last_update == 0:
        return
    secs[cpu] += last_update - start_time
    execs[cpu] += execs_done

def do_work(cpu):
    master_arg = "-M"
    if cpu != 0:
        master_arg = "-S"

    # Restart if it dies, which happens on startup a bit
    while True:
        global stop_flag
        global stopped
        if stop_flag is True:
            stopped[cpu] = True
            return
        try:
            sp = subprocess.Popen([
                "taskset", "-c", "%d" % cpu,
                "afl-fuzz", "-i", INPUT_DIR, "-o", OUTPUT_DIR,
                master_arg, "fuzzer%d" % cpu, '--',
                "./djpeg", "@@"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            sp.wait()
        except:
            pass

        # print("CPU %d afl-fuzz instance died" % cpu)
        update_stats(cpu)

        # Some backoff if we fail to run
        # time.sleep(1.0)

assert os.path.exists(INPUT_DIR), "Invalid input directory"

if os.path.exists(OUTPUT_DIR):
    # print("Deleting old output directory")
    shutil.rmtree(OUTPUT_DIR)

# print("Creating output directory")
os.mkdir(OUTPUT_DIR)

# Disable AFL affinity as we do it better
os.environ["AFL_NO_AFFINITY"] = "1"

for cpu in range(0, NUM_CPUS):
    threading.Timer(0.0, do_work, args=[cpu]).start()

    # Let master stabilize first
    if cpu == 0:
        time.sleep(1.0)
    else:
        time.sleep(0.5)

d1 = datetime.now()
while threading.active_count() > 1:
    time.sleep(10)
    d2 = (datetime.now() - d1).total_seconds()
    if d2 > EXEC_TIME:
        break

    print('\rdone: %d/%d' % (d2, EXEC_TIME), end='')
    # v = ""
    # try:
    #     v = subprocess.check_output(["afl-whatsup", "-s", OUTPUT_DIR])
    # except:
    #     pass
    # print(v)

def kill_again_and_again():
    try:
        subprocess.check_call(['pkill', '-9', 'afl'])
    except:
        pass


print('')
stop_flag = True
time.sleep(1)
subprocess.check_call(['pkill', '-9', 'afl'])
time.sleep(0.1)
for i in range(1000):
    kill_again_and_again()
time.sleep(2)
total_seconds = sum(secs)
total_execs = sum(execs)
if total_seconds != 0:
    print("%3d\t%10.3f" % (NUM_CPUS, total_execs * 1.0 / total_seconds))
else:
    print("%3d\t%10.3f" % (NUM_CPUS, total_seconds))
