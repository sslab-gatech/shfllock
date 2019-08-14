Scalable and Practical Locking with *Shuffling*
===============================================

### Paper
* [Scalable And Practical Locking with Shuffling (SOSP 2019)]()

### Overview
ShflLocks are a family of locking protocols that
implement use shuffling mechanism to implement several policies,
such as NUMA-awareness, or blocking. We implement three locking
protocols: NUMA-aware spinlock, NUMA-aware blocking mutex,
and NUMA-aware blocking rwlock.

### Tested Environment
- We use Ubuntu 16.04 but with our versions of kernels for testing purposes.

### Required programs
- We use the following programs for evaluation
  - Fxmark: file system micro-benchmarking in the kernelspace
  - lock1: This benchmark stresses the kernel spinlock and is part of the will-it-scale benchmark suite
  - Exim: mail server to stress kernelspace locks
  - Metis: map-reduce library to stress readers-writer lock in the kernel
  - AFL: fuzzer to stress kernel locks
  - Dedup: Parsec benchmark that stresses lock allocation in the usersapce
  - Streamcluster: Parsec benchmark that has about 40% of `trylock` calls in the userspace
  - Leveldb: Database benchmark that stresses a single lock in the userspace
  - Extended version of RCU-table benchmark, used for nano-benchmarking locks.


### Kernelspace Benchmark
- To test locks in the kernelspace, patch the kernel in the `patches` folder
- Clone the following benchmark suite:
```bash
  $ git clone https://github.com/sslab-gatech/vbench
  $ git clone https://github.com/sslab-gatech/fxmark
```
- We use four benchmarks from Fxmark and two from Vbench.
- Fxmark:
  - MWCM
  - MWRL
  - MWRM
  - MRDM
- Vbench:
  - Exim
  - Metis
- AFL: Follow the README.md in the fuzzing directory
- Nanobenchmark: Please refer to the `kernel-syncstress` for more information.

### Userspace Benchmark

- We extend the `Litl` framework (`rwlocks/src/litl/`) and use the `LD_PRELOAD` for usersapce benchmarks.

### Contact
- Sanidhya Kashyap (sanidhya@gatech.edu)
