# Dioptase OS

Operating system for the [Dioptase system](https://github.com/b-Rocks2718/Dioptase)

## Makefile usage

The Makefile in `Dioptase-OS/` builds a test program from a root C file and
links it with kernel sources to produce a kernel-mode hex image in `build/`.

### Requirements

These tools are expected to be built already:

- `../Dioptase-Languages/Dioptase-C-Compiler/build/debug/bcc`
- `../Dioptase-Assembler/build/debug/basm`
- `../Dioptase-Emulators/Dioptase-Emulator-Full/target/debug/Dioptase-Emulator-Full`

### Targets

- `make <test>.hex` compiles `Dioptase-OS/<test>.c`, compiles all `kernel/*.c`,
  assembles them along with any `kernel/*.s`, and writes `build/<test>.hex`.
- `make <test>` does the same build and then runs the full emulator on the hex.
- `make clean` removes `build/*.hex` and temporary assembly outputs.

### Notes

- Generated assembly files are named `*.s` and kept after assembly.
- Kernel C assembly outputs live under `build/kernel/` to keep them separate
  from the root test's assembly file.
- The makefile also compiles the BIOS. The emulator is run with the BIOS in ram and the kernel on the sd card, the bios loads the kernel and then jumps to it
