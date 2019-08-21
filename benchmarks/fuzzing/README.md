# AFL

- We use libjpeg as a fuzzing target [1].

## Setup
- Compile the afl and the instrumentation part as follows:
```bash
 $ make -C afl
 $ make -C afl/llvm-mode
```
- Note that you will need LLVM installed with `$LLVM_CONFIG` and `$PATH` configured.

- Setup the libjpeg as follows:
```bash
 $ AFL_DIR=$(pwd)/afl
 $ mkdir buildjpeg
 $ cd buildjpeg
 $ export PATH=$PATH:$AFL_DIR
 $ cmake -G"Unix Makefiles" -DCMAKE_C_COMPILER=afl-gcc -DCMAKE_C_FLAGS=-m32 ../libjpeg-turbo
 $ make -j
```

## Executing
- Execute the `run-afl.sh` script to prepare and run the fuzzing
```bash
 $ ./run-afl.sh
```

### Reference
[[1] Scaling AFL to a 256 thread machine](https://gamozolabs.github.io/fuzzing/2018/09/16/scaling_afl.html)
