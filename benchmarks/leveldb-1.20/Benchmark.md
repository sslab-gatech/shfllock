### LevelDB

- First compile the jemalloc present in `benchmarks` folder.
```bash
 $ cd ../jemalloc/autogen.sh
 $ ./autogen.sh
 $ ./configure --without-export --disable-libdl
 $ make -j
 $ cd ../leveldb-1.20
```
- Now, compile LevelDB with `make`.
- Please execute the `run.sh` after compiling the `Litl` Library.

- `run.sh` requires two command line arguments:
```bash
# $1: number of threads
# $2: experiment duration (in milliseconds)
```
