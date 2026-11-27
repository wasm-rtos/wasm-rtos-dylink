# wasm-rtos-dylink

**`dylink.0` shared libraries for [wasm-rtos](https://github.com/wasm-rtos/wasm-rtos)
and [wasm-rtos-vm](https://github.com/wasm-rtos/wasm-rtos-vm).** A
position-independent program (`clang -pie`) and the libraries it needs
(`clang -shared`) run as one process: one linear memory, one function
table, calls between them straight into each other's code, as with ELF
shared objects. The format is the tools' convention,
[DynamicLinking.md](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md),
followed as `wasm-ld` writes it.

```c
os_init();
os_dylink_add();                                        /* the loader joins the kernel */
os_dylink_add_library("libui.so", ui_bytes, ui_size);   /* a library, by the name programs need */
os_task_create(&task, app_bytes, app_size, "main", "app", 0, OS_TASK_PRIORITY_NORMAL);
```

A build that does not call `os_dylink_add` has none of this: the linker
leaves it out, as it leaves out a driver nobody registers.

## What it is made of

| File | What it is |
|---|---|
| `dylink.c`, `dylink.h` | The convention on the interpreter's public `wm.h` alone: any host can use it |
| `os_dylink.c`, `os_dylink.h` | The kernel's loader (`OsLoader`, wasm-rtos decision 16) and the libraries by name |

It is one way of making a program out of modules, joined to the kernel
the way drivers are; another way is written the same way, on the same
functions (wasm-rtos-vm decision 23: `wm_module_imports`,
`wm_instance_link_function`, `wm_table_set`...).

## On the interpreter alone

```c
DylinkOptions options = {context, find_library, host_resolver, host_context, 0};
Dylink* process;
WmResult r = dylink_build(&process, program, &options);   /* instantiated and linked, nothing run */
if (!r)
    r = dylink_start(process, runtime);                    /* start functions, relocations, constructors */
WmInstance* app = dylink_instance(process);
...
dylink_free(process);
```

## Saving a linked program

The program's instance is saved with every instance linked to it (a call
stopped inside a library, the shared memory, table and globals), and read
back into a process built again from the same program and libraries:
`os_task_save_snapshot`, `os_system_save` and deep sleep work for a linked
task as for any. A library that is not the one saved is refused. Linked
tasks do not swap out yet (`OS_STATUS_UNSUPPORTED`).

## Building and checking

Add `dylink.c` and `os_dylink.c` to a wasm-rtos build. `tools/check.sh`
runs the test on the kernel next to this repository (`KERNEL`, by default
`../wasm-rtos`, and its `vm/`, or `VM`); `test/build.sh` builds the
test's program and library with clang and `wasm-ld`, which are kept next
to their sources.

Released into the public domain (the Unlicense), as the rest of wasm-rtos.
