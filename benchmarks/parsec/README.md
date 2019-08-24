## Parsec

- We use Dedup and Streamcluster from the Parsec benchmark suite.
- Follow this [parsec link](https://parsec.cs.princeton.edu/download.htm) for the setup and datasets for Dedup and Streamcluster.
- Now, copy the `run.sh` in the Parsec directory and execute the following:
```bash
$ ./run.sh FC-6-x86_64-disc1.iso # path of an iso image to be used for dedup
```
- Note: we assume that parsec programs are present in the current folder.
  If that is not the case, please update the relative/absolute path to
  `ulocks/src/litl` for the `LOCK_DIR` variable in the `run.sh` file.
  Check line 10 in `run.sh` for the reference.
