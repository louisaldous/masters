# Exploring the Potential of Speculative Parallelisation

This is the software repo for my master's thesis on speculative parallelisation, and exploring the impact a software implementation of it has on a given workload. The aim of this is to explore the areas where a hardware-accelerated implementation would have the most impact, and to provide a basis for future research in the area using this implementation.

The directory structure is as follows:
- llvm-plugin/ 
  - LoopExtraction/ -> contains the source code for LoopExtractionPass
  - InstrumentFunction/ -> contains the source code for InstrumentFunctionPass
- threadlib/ -> contains the source code and test files for the thread library

## Compiling

For the LLVM passes, place the llvm-plugin folder into the llvm/ folder in the LLVM repo root directory. Modify llvm/CMakeLists.txt to add the folder as a sub-directory. When configuring CMake, add the `-DLLVM_LOOPEXTRACTION_LINK_INTO_TOOLS=ON -DLLVM_INSTRUMENTFUNCTION_LINK_INTO_TOOLS=ON` options to statically link the passes into the LLVM toolchain. Compile LLVM as normal.

For the thread library, run `make` to compile the library with debug flags and thread sanitization on by default. `RELEASE` can be defined as a variable to compile with `-O3` and other performance flags. Other targets are also available in the `Makefile`.

## Using the Implementation

To enable speculative parallelisation when compiling add the `--enable-extract-loop-bodies` option. `-flto` needs to be enabled to instrument the produced code, which means that the gold linker with plugin support needs to be used and configured on the host machine. Linking must be done with `-lthreadlib`. `LD_LIBRARY_PATH` must include the path to the compiled thread library for the program to run.

NOTE: `--enable-extract-loop-bodies` is to be passed to LLVM, so programs such as `clang` may require an additional command-line argument to do this (in this case `clang` would require `-mllvm` first).
