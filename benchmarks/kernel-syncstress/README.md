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
vermagic:       5.2.8-200.fc30.x86_64 SMP mod_unload 
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

- One can vary the read-write ratio with `rw_writes`, which is 1 by default.
