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
  read back as any; it does not swap out yet.
- A function import a library re-exports from the host is linked; it was
  left unresolved.
