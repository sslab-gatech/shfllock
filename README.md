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


### Userspace Benchmark


### Kernelspace Benchmark
- To test locks in the kernelspace, patch the kernel in the `patches` folder
- Clone the following benchmark suite:
```bash
  $ git clone https://github.com/sslab-gatech/vbench
  $ git clone https://github.com/sslab-gatech/fxmark
```


### Contact
- Sanidhya Kashyap (sanidhya@gatech.edu)
