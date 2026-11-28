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
