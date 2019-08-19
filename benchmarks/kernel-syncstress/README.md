# Benchmark locks in the kernel space

- This folder contains a hash table nano-benchmark for stressing various locks.

- Please, compile the kernel module as follows:
```bash
 $ make
```
- It will generate `ht.ko` kernel module in the `build` folder.
- Run the `./run-ht.sh` script that will run with various locks for nano-benchmarking.
- The kernel module takes several arguments as shown:
```bash
license:        GPL
description:    Simple stress testing for spinlocks.
author:         XXX
depends:
retpoline:      Y
name:           ht
vermagic:       XXX
parm:           reader_type:Hash table reader implementation (charp)
parm:           writer_type:Hash table writer implementation (charp)
parm:           ro:Number of reader-only threads (int)
parm:           rw:Number of mixed reader/writer threads (int)
parm:           rw_writes:Number of writes out of each total (ulong)
parm:           rw_total:Total rw operations to divide into readers and writers (ulong)
parm:           buckets:Number of hash buckets (ulong)
parm:           entries:Number of hash table entries (ulong)
parm:           reader_range:Upper bound of reader operating range (default 2*entries) (ulong)
parm:           writer_range:Upper bound of writer operating range (default 2*entries) (ulong)
```

- We can vary the read-write ratio with `rw_writes`, which is 1 by default.
- Each execution looks as follows:
```bash
[11636.211506] rcuhashbash starting threads
[11656.340936] rcuhashbash summary: crit: 0 offpath: 19 (34104759 50173422)
[11656.340983] rcuhashbash summary: ro=0 rw=12 reader_type=table_aqm_mutex_fp writer_type=table_aqm_mutex_fp
               1rcuhashbash summary: writer proportion 1/100
               1rcuhashbash summary: buckets=1024 entries=4096 reader_range=8192 writer_range=8192
               1rcuhashbash summary: writes: 210699 moves, 211243 dests in use, 421592 misses (843534)
               1rcuhashbash summary: reads: 41719771 hits, 41714876 misses (83434647)
               1rcuhashbash summary: total: 84278181 (avg: 20006397626 min: 20006280752 max: 20006452620)
[11656.341150] rcuhashbash done
```
- The last two numbers in the second line `(34104759 50173422)` is the sum of number of operations
  by first half of threads and the second half of threads after sorting.
- The second line also tells how many sleeping waiters were woken in the critical path (`crit: 0`),
  while how many were woken entirely of the critical path (`offpath: 19`).
- To measure the breakdown of every optimization, select/deselect the following flags in `locks/aqs.c` file:
```C
#define ENABLE_SHUFFLERS                        1
#define ALLOW_PREDECESSORS_AS_SHUFFLERS         1
#define SELECT_SLEADER                          1
```
