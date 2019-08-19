# AFL

- We use libjpeg as a fuzzing target, which is in the `afl-test` folder.

## Setup
- Compile the afl and the instrumentation part as follows:
```bash
 $ make -C afl
 $ make -C afl/llvm-mode
```
- Note that you will need LLVM installed with `$LLVM_CONFIG` and `$PATH` configured.

- Setup the libjpeg as follows:
```bash
 $ cd jpeg-9b
 $ CC=../afl/afl-gcc ./configure
 $ make
 $ ./djpeg -h # for creating a executable used for fuzzing in ./libs/lt-djpeg
```

## Executing
- Execute the `run-afl.sh` script to prepare and run the fuzzing
```bash
 $ ./run-afl.sh
```
