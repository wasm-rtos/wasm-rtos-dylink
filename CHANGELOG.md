# Changelog

## Unreleased

- **dylink.0 out of the interpreter and the kernel** (wasm-rtos decision
  16, wasm-rtos-vm decision 23): what was `wm_dylink.c` in wasm-rtos-vm,
  written on its public `wm.h` (`dylink_build`, `dylink_start`,
  `dylink_instance`, `dylink_free`), and a loader of the kernel
  (`os_dylink_add`, `os_dylink_add_library`) in place of `OS_HAS_DYLINK`
  and `os_wasm_library_register`.
- Built in two steps, as the interpreter instantiates: nothing runs while
  the process is built, its start functions, relocations and
  constructors run after. A linked task is saved, put to deep sleep and
  read back as any, and swaps out as any (kernel decision 17): back from
  swap the program is built on its memory kept apart and the libraries
  bound to it, nothing written into it, nothing started again
  (`DylinkOptions.memory`).
- A function import a library re-exports from the host is linked; it was
  left unresolved.
- **Libraries from a directory of the file system** (decision 1):
  `os_dylink_set_directory("/sd/lib")`, and a program needing `libui.so`
  links `/sd/lib/libui.so`: mapped where the file system can (run in
  place), read into RAM otherwise, one copy for the processes linked with
  the same contents, held until a process is freed (swapped out or
  ended). New libraries on the card need no new firmware.
