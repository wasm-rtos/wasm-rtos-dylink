# Decisions

What wasm-rtos-dylink decided, why, and what was built. The kernel's
decisions 16 and 17 (wasm-rtos) and the interpreter's 23 and 24
(wasm-rtos-vm) made this repository; they are not repeated here.

## 1. Libraries from a directory of the file system

**Decided and built (2026-11-28).** The user: «пусть сразу как
/sd/lib/.so просто почему не написать к файлу путь?»; against a search
through directories: «дак зачем поиск если можно сразу путь указать?»;
and, after a path for each library and libraries named by programs were
weighed: «значит решение с директорией лучшее да? надо прописать почему
не из программ покрайне мере пока что можно благодаря директории не
обновляя прошивку добавлять новые».

### Problem

`os_dylink_add_library` takes a library as bytes in memory. A library in
flash the processor reads runs in place that way; a library on an SD
card does not run at all, unless the host reads it into RAM itself and
keeps it there for good. A program on the card runs by its path
(`spawn-path`, kernel decision 9); the libraries it needs cannot be put
next to it.

The program cannot name the path itself: `wasm-ld` writes a needed
library as its file name alone (`libui.so`, whatever path it was linked
from; `--soname` changes only the name section). Writing a path into the
`dylink.0` section would take a tool of our own after the linker, so the
same `.wasm` would no longer come from any compiler as it is.

### Decision

**The host names one directory; a needed library is the file of that
name in it.** No search: the path is the directory and the name, one
file opened.

```c
/*
 * The directory of libraries: a program (or a library) needing libui.so
 * gets <path>/libui.so, unless a library was added by bytes under that
 * name. The path stays the caller's; called again, it replaces the one
 * before for programs linked after. With the kernel's file system only.
 */
OsStatus os_dylink_set_directory(const char* path);
```

```c
os_fs_mount("/sd", &sd_driver, &sd);
os_dylink_add();
os_dylink_set_directory("/sd/lib");      /* libui.so is /sd/lib/libui.so */
os_task_spawn_path(&task, "/sd/apps/app.wasm", NULL, 0, 0, 0);
```

- **New libraries without new firmware**: a `.so` copied into the
  directory is linked by every program that needs it, on every board,
  with no line of the firmware changed. A library that needs another
  finds it the same way.
- **Opened when linked**: nothing is read when the directory is set; a
  library no program runs takes no RAM, and a file replaced on the card
  is seen by the next program linked.
- **Opened the way programs are**: as `spawn-path` opens a program
  (`os_fs_image_open`): mapped when the file system can map it (a
  library in flash runs in place, no copy), read into RAM otherwise.
- **Shared by contents**: processes linked with a file of the same size
  and the same bytes (compared in pieces, `os_fs_file_matches`, before
  anything is read whole) share one copy; a file replaced on the card is
  a new copy, the processes linked before keep theirs. As kernel decision
  9 shares programs.
- **Held while linked**: a process holds its libraries' files from its
  build to its free. Swapping a task out frees its process (kernel
  decision 17), so a swapped-out task holds no library in RAM; back from
  swap the file is opened and read again. A library that changed while
  the task was out is not the one saved: the interpreter's check refuses
  it (`wm_snapshot_invalid`, wasm-rtos-vm decision 23), never linked over
  the kept memory. The host's `os_task_swap_in` then answers the error and
  keeps the task's state (it can come back once the file is the same: a
  card taken out and put back); the scheduler bringing it back ends the
  task with the error, as for any task that cannot come back (the
  kernel's rule, unchanged here). Deep sleep the same.
- **A name is a file name**: a needed name with `/` in it, or `.` or
  `..`, is not opened in the directory (it cannot leave it).
- **Bytes first**: a library added by bytes under the name is taken
  before the directory's file, so a board can put one in flash over the
  card's.
- At most `OS_DYLINK_MAX_FILES` (8) library files are open at once; one
  more is unlinkable.
- Built only with the kernel's file system (`OS_HAS_FS`); `os_dylink.c`
  includes the kernel's `fs.h` for it, as the host does to mount. A build
  that never calls `os_dylink_set_directory` carries none of the files'
  code (it sets the two functions the loader calls), as kernel decision 3
  has it.

### Why

- A program and its libraries on the card are then files and nothing
  else: the firmware says once where libraries live, the card says which
  there are.
- One directory is not a search: each name is one path, one file opened.
- It is the program path's own way (open, map or read, share by
  contents), so a library in flash or on a card behaves as a program
  there does.
- Holding files only while linked gives RAM back on swap, the point of
  paging a task out.

### Instead

- **The path in the program**: the linker does not write one (see
  Problem); a tool of our own to write it would make the file depend on
  our build.
- **A path for each library** (`os_dylink_add_library_file("/sd/lib/libui.so")`):
  the clearest to read, but each new library is a line of firmware and a
  board updated.
- **Several directories looked through in turn**: a search, for a choice
  nobody needed yet; a library in flash over the card's is the one added
  by bytes.
- **Libraries named by programs, not by the firmware — not now.** A
  program's libraries are linked before any of its code runs (`build`,
  then `start`), so a program cannot say where its own are. What is left:
  - a program (a shell, the first program started) sets libraries for
    the programs after it: a new ABI call, so ABI 1.9 (1.x is frozen at
    1.8, kernel decision 8), and a rule for which program may change what
    others link;
  - a program opens a library by path while it runs and takes its
    functions by name: the memory and the table grow while it runs, and
    swap and snapshots would have to open the same libraries again in the
    same order.
  The directory already lets new libraries come without new firmware;
  either of these can come later on top of it, with a decision of its
  own.
- **Read every library when the directory is set**: RAM for libraries
  nobody runs, and a file changed on the card never seen.
- **Hold the files across swap**: a swapped-out task would keep its
  libraries' copies in RAM.

### Steps

| # | What | Where | Status |
|---|---|---|---|
| 1 | `os_dylink_set_directory`; the directory's file after the libraries by bytes; names that are not file names refused | `os_dylink.c`, `os_dylink.h` | done |
| 2 | Open library files: opened through `os_fs_image_open` when linked, shared by contents, held by each process and released by its free | `os_dylink.c` | done |
| 3 | Test: a library from a file system that maps it (run in place, nothing read) and from one that cannot (read); nothing opened when the directory is set; two processes share one copy; their processes freed, the copy goes; swapped out the copy goes and back it is read again; a file changed while swapped out is refused (`os_task_swap_in` answers it, the state kept) and the task comes back once it is the same; bytes before the directory's file; a needed name that is a path not opened; a missing file is unlinkable; each rule broken on purpose fails the test | `test/test.c` | done |
| 4 | Boards: a program and its library from the SD card on the ESP32, swapped out and back | board tests (local) | done |
| 5 | README, CHANGELOG, Roadmap | all | done here; Roadmap |

### Built

As on paper. A process of the loader is now a small record of its own
(the linked instances and a mask of the files it holds), so files can be
released when it is freed. Checked: `tools/check.sh`, 48 checks in each
of its three builds (sanitizers, and every memory paged); broken on
purpose, one at a time (no sharing by contents, no release on free, the
directory before the libraries by bytes, no file-name check, a relative
directory accepted), each fails the test. Not checked here: a library
that needs another from the directory (the test has one library; the
dependencies are followed as for libraries by bytes), and more than
`OS_DYLINK_MAX_FILES` open at once.

On the ESP32 (no PSRAM, SD card over SPI, a swap file of 64 frames on
the card and 4 in RAM; the test's program and library as they are, 64 KiB
pages): the program from the card by its path, its library from
`/sd/lib`, linked in 67 ms; two tasks of it, one copy of the library; a
task stopped inside the library swapped out (3 frames to the card) and
back in 57 ms, its library read again; a library changed on the card
while it was out refused (`snapshot does not fit this module or
runtime`), the task back once the file was the same. Twice in a row, the
same free heap after each (no leak).

Sizes on a Cortex-M4 (`-Os`, sections the linker keeps): with no
directory set, +128 bytes of code and +8 of RAM over the loader before
(the process record, two pointers); with `os_dylink_set_directory`
called, +554 bytes of code and +228 of RAM (the table of open files).

## 2. A library is parsed once, for every process linked with it

**Decided and built (2026-11-28).** Hole (a) of the list after the swap.

### Problem

Each process parses its own module of every library it links: `find`
answers `wm_module_load` over the library's bytes each time, and
`dylink_free` frees the module with the process. The bytes are shared
(decision 1 for files; the caller's bytes for libraries added by bytes),
the parsed module is not. Measured (64-bit): the test's library, 1261
bytes, is 2032 bytes of module in each process; a program of the size of
the tour (40 KB with `printf`) is 31 KB of module (wasm-rtos-vm decision
7), and a library of that size would cost as much in every process
linked with it. Kernel decision 9 stopped this for programs; libraries
still pay it.

### Decision

**A library's module is loaded once and shared**, as kernel decision 9
shares a program's.

- **dylink.c**: `DylinkOptions` gains `release(context, module)`, its
  last field (an initializer written before still builds), called by
  `dylink_free` for each library's module instead of `wm_module_free`
  (the instances go first, as now). `find`'s module is then lent, not
  given. Without `release`, as now: the process owns what `find` gives.
  `dylink_free` keeps the context it needs for it.
- **os_dylink.c**: each library the loader knows (added by bytes, or an
  open file of the directory) keeps its module and a count of the
  processes using it; `find` loads it on the first and lends it after,
  `release` frees it on the last. A swapped-out task holds none (its
  process is freed, kernel decision 17); back from swap it takes the
  module again, or loads it if nobody had it.
- **The same file under two names** in one process (two needed names, one
  contents): the second gets a module of its own, as two libraries did
  before; one module cannot be two objects of a process.
- **What stays per process**: instances, the memory and table, globals,
  the GOT: everything `dylink_build` makes.
- **Nothing in the interpreter or the kernel changes**: instances of one
  module already share it (kernel decision 9).

### Why

- A module and its decoded code never change while instances use them;
  the interpreter already lets instances of one module share it (kernel
  decision 9 runs on that).
- The library's code is the bulk of a process that links it; a second
  process of the same program then costs its memory and state, not the
  library again.

### Instead

- **Share in dylink.c itself** (a cache of modules by bytes): dylink.c
  works on any host, and which bytes are the same library is the host's
  knowledge (by name, by file contents).
- **A module that counts its own users in the interpreter**: one more
  rule in `wm.h` for what a counter in the loader does.
- **Keep the module while no process uses it**: RAM for a library
  nobody runs; it is parsed again when needed, as a program is.

### Steps

| # | What | Where | Status |
|---|---|---|---|
| 1 | `DylinkOptions.release`; `dylink_free` lends modules back | `dylink.c`, `dylink.h` | done |
| 2 | Libraries by bytes and files of the directory keep a shared module with its count | `os_dylink.c` | done |
| 3 | Test: two processes of the program, one module of the library; freed, it goes; swapped out, it goes and comes back; a file changed is a new module, the old one stays with its processes; dylink.c alone without `release` as before; each rule broken on purpose fails the test | `test/test.c` | done |
| 4 | Boards: the heap of two linked tasks before and after (ESP32 with the card) | board tests (local) | done |
| 5 | README, CHANGELOG | here | done |

### Checks

- `tools/check.sh`, 64 checks, each of the three builds. The test counts
  the interpreter's memory (`wm_set_allocator`): a second task of the
  program costs the library's module less than the first (1912 bytes on
  a 64-bit computer, the module predecoded), by bytes, from a mapped file
  and from a file read into RAM, and the three cost the same; the first
  freed, the second runs on; the last freed, the memory is as before; a
  task made while the only linked one is swapped out costs as the first
  did; a file changed while a process runs gets a new module and both
  run to their end. The test's page store grew to 128 frames: 64 did not
  hold a second task beside one swapped out.
- Each rule broken on purpose fails the test (10): `dylink_free` freeing
  lent modules; `release` not called; a library parsed per process, by
  bytes or from a file; a module never freed, by bytes or from a file;
  a lent module freed by `release`, either kind; users not counted; the
  loader passing no `release`.
- Cortex-M4 (`-Os`, both objects whole): +34 bytes of code in `dylink.c`,
  +256 in `os_dylink.c`; +160 bytes of RAM (a module and a count for each
  of the 16 libraries, a module for each of the 8 files).
- ESP32 with the card, the test's program and library from it, run twice
  in a row, before and after: every check passes (16). The second linked
  task takes 26676 bytes of heap, 28248 before (−1572, the library's
  module); the first 29740 in both. Linked from the card in 70 ms (66),
  back from swap in 58 ms (57). The free heap after shutdown is the same
  after each run (250140): nothing leaks; 160 bytes less than before,
  the loader's tables in RAM.
- **Not with `OS_HAS_MEMORY_ACCOUNTING=1`**: the module is charged to the
  task that parsed it, and freed after that task is gone the count is
  written into freed memory (AddressSanitizer). The kernel's own shared
  programs (kernel decision 9) do the same: two tasks spawned of one
  program, the first deleted, then the second. A kernel matter, open.
