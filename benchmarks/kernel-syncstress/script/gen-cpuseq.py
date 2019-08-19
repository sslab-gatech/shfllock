#!/usr/bin/python

import errno
import sys
import os

cpuinfo = [dict(map(str.strip, line.split(":", 1))
                for line in block.splitlines())
           for block in file("/proc/cpuinfo", "r").read().split("\n\n")
           if len(block.strip())]

# Keep only primary hyperthreads
primaries = set()
for cpu in cpuinfo:
    processor = cpu["processor"]
    try:
        s = file("/sys/devices/system/cpu/cpu%s/topology/thread_siblings_list" % processor).read()
    except EnvironmentError, e:
        if e.errno == errno.ENOENT:
            primaries.add(processor)
            continue
        raise
    ss = ''
    try:
        ss = set(map(int, s.split("-")))
    except:
        ss = set(map(int, s.split(",")))
    if int(processor) == min(ss):
        primaries.add(processor)
cpuinfo = [cpu for cpu in cpuinfo if cpu["processor"] in primaries]

def seq(cpuinfo):
    packages = {}
    package_ids = set()
    for cpu in cpuinfo:
        if "physical id" in cpu:
            package_id = int(cpu["physical id"])
            packages.setdefault(package_id, []).append(cpu)
            if cpu["processor"] is "0":
                cpu0_package_id = int(package_id)
            package_ids.add(int(package_id))
        else:
            yield cpu
    for cpu in packages[cpu0_package_id]:
        yield cpu
    package_ids.remove(cpu0_package_id)

    for package_id in package_ids:
        for cpu in packages[package_id]:
            yield cpu

if __name__ == "__main__":
    cpu_count = len(cpuinfo)
    with open("include/cpuseq.h", "w") as f:
        f.write("int online_cpus = %s;\n" % cpu_count)
        f.write("int cpuseq[] = { %s" % ",".join(cpu["processor"] for cpu in seq(cpuinfo)) +" };")
