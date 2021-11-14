### Requirement

- Please checkout the Linux v4.19-rc4 version of the Linux kernel
```bash
  $ git clone --branch v4.19-rc4 https://github.com/torvalds/linux
  $ cd linux
```

- Use monkey patching to apply these patches for running Linux with different versions.
```bash
  $ patch -p1 < <path-to-path>
```
