Scalable and Practical Locking with *Shuffling*
=============================================

## Paper
* [Scalable And Practical Locking with Shuffling (SOSP 2019)](https://gts3.org/assets/papers/2019/kashyap:shfllock.pdf)

## Overview
ShflLocks are a family of locking protocols that
implement use shuffling mechanism to implement several policies,
such as NUMA-awareness, or blocking. We implement three locking
protocols: NUMA-aware spinlock, NUMA-aware blocking mutex,
and NUMA-aware blocking rwlock.


## ShflLocks in kerenelspace

To test locks in the kernelspace, use a patch in the `klocks` folder.
The patch is written on top of linux v4.19

- Please checkout the Linux v4.19-rc4 version of the Linux kernel
```bash
  $ git clone --branch v4.19-rc4 https://github.com/torvalds/linux
  $ cd linux
```

- Use monkey patching to apply these patches for running Linux with different versions.
```bash
  $ patch -p1 < <path-to-path>
```

## ShflLocks in userspace

We extend the `Litl` framework (`ulocks`) and use the `LD_PRELOAD` for usersapce benchmarks.

### Supported algorithms

| Name | Ref | Waiting Policy Supported | Name in the Paper [LOC] | Notes and acknowledgments |
| ---  | --- | --- | --- | --- |
| **AQS** | [NUMA-MCS] | original (spin) | non-block shfllock | ShflLock paper |
| **AQS-WO-NODE** | [NUMA-MCS] | spin | non-block shfllock wo node | ShflLock paper |
| **AQM** | [NUMA-MUT] | spin_then_park | blocking shfllock | ShflLock paper |
| **AQM-WO-NODE** | [NUMA-MUT] | spin_then_park | blocking shfllock wo node | ShflLock paper |

### How to run

Compile the `Litl` framework as follows:

```bash
make -C ulocks
```

If you want to use non-blocking ShflLock, do the following:

``` bash
./libaqs_spinlock.sh my_program
```

You can find more details in `ulocks/README.md`.


### Contact
- Sanidhya Kashyap (sanidhya@gatech.edu)
