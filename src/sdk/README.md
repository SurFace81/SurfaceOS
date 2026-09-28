# The SurfaceOS SDK

How to write a program for SurfaceOS. Each function is described in its
header, next to its declaration (`include/sfos/*.h`); this page is the
overview.

## A program

A program is one C++ file (`src/apps/<name>.cpp`) or a folder of them
(`src/apps/<name>/`). It includes one header and implements `SfMain`:

```cpp
#include <sfos.h>

extern "C" SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    SfConsole* Con = Sys->Console;
    Con->Print(Con, "Hello from ");
    Con->Print(Con, App->Name);
    Con->Print(Con, "\n");
    return SF_SUCCESS;
}
```

`make surfaceos.img` builds it into `bin/apps/<name>.bin` and puts it on
the boot volume as `/apps/<name>` (no extension). The console runs it by its name:
`hello`, `& hello` (in the background), `sudo hello` (with the admin
right).

There is no libc and no start-up code. Programs are built freestanding
(`-ffreestanding -fno-exceptions -fno-rtti -nostdlib`) and linked with
`src/sdk/sfos.ld`. The kernel starts the program in the SDK runtime, which
calls `SfMain`. What `SfMain` returns ends the program: every thread in it
ends, and the returned value is the status its parent gets from `Wait`.

## Tables

Everything a program can do goes through tables of function pointers,
all reached from `Sys`. Each function takes its own table as the first
argument:

```cpp
Sys->Console->Print(Sys->Console, "text\n");
File->Read(File, Buffer, &Size);
```

Every table starts with a header (`sfos/table.h`):

| Field       | What it holds                                              |
|-------------|------------------------------------------------------------|
| `Signature` | eight characters naming the table (`SF_CONSOLE_SIGNATURE`) |
| `Revision`  | `SF_REVISION(major, minor)` of the layout the system filled in |
| `Size`      | the size in bytes of the table as filled in                |

**Every table is at revision 1.0.** From now on a table only grows at its
end:

- A new function or field goes after the last one, and the minor revision
  goes up.
- Nothing is moved, removed or changed in meaning. A program built with an
  older SDK keeps working on a newer system.
- A program that wants something added later checks first that the system
  it runs on has it:

  ```cpp
  if (SF_HAS_FIELD(Sys, SfSystem, Admin) && Sys->Admin)
      ...
  ```

  `SF_HAS_FIELD` compares `Hdr.Size` with where the field ends. Anything
  that is in revision 1.0 needs no check.

Every header also pins its layout with `SF_STATIC_ASSERT`. A change that
would move a field breaks the build instead of every program built before
it. `sdkcheck` checks at run time that each table the system hands out
has the signature, revision and size of the headers it was built with.

## Status

Every call returns an `SfStatus`: `SF_SUCCESS` (0), or an error with the
top bit set (`SF_ERROR(s)`). Results come back through pointer arguments.
The statuses are defined in `abi/sfcall.h`.

| Status                                     | Meaning                                                      |
|--------------------------------------------|--------------------------------------------------------------|
| `SF_INVALID_PARAMETER`                     | a bad argument, such as a pointer the program does not own   |
| `SF_NOT_FOUND`                             | no such file, program or device                              |
| `SF_ACCESS_DENIED`                         | not allowed                                                  |
| `SF_OUT_OF_RESOURCES`                      | out of memory or of free slots                               |
| `SF_ABORTED`                               | cut short, for example by Ctrl+C in `ReadLine`               |
| `SF_END_OF_FILE`                           | nothing more to read                                         |
| `SF_BUFFER_TOO_SMALL`                      | the result does not fit; the size needed comes back          |
| `SF_IN_USE`                                | busy, for example a volume with files open                   |
| `SF_CRASHED`                               | the program was ended by a CPU exception                     |
| `SF_UNSUPPORTED`, `SF_TIMEOUT`, `SF_BAD_HANDLE`, `SF_ALREADY_EXISTS`, `SF_DEVICE_ERROR` | as their names say |

A program's own `SfMain` may return any status. `SF_ERROR_BIT | n` is
fine for a program-specific error.

## What `Sys` holds

| Field          | Header          | What it does |
|----------------|-----------------|--------------|
| `Sys->Console` | `sfos/console.h` | The program's screen and keyboard. |
| `Sys->Files`   | `sfos/files.h`, `sfos/file.h` | Files and folders under the program's roots. |
| `Sys->Memory`  | `sfos/memory.h` | Whole pages, and a heap (`Allocate`/`Free`). |
| `Sys->Time`    | `sfos/time.h`   | The clock, the time since boot, `Sleep`. |
| `Sys->Process` | `sfos/process.h` | Start other programs and wait for them. |
| `Sys->Thread`  | `sfos/thread.h` | More threads in the same program. |
| `Sys->Sync`    | `sfos/sync.h`   | Mutexes and events between threads. |
| `Sys->Admin`   | `sfos/admin.h`  | Only with the admin right, `nullptr` otherwise. |

`App` (`sfos/app.h`) describes the program itself: its `Name` and its
command line (`ArgCount`, `Args`; `Args[0]` is the program).

### Console

The screen is a grid of `Columns` x `Rows` cells below the system's title
bar. The console has two modes:

- **Line mode** (the default) scrolls text. `Print` writes it, and
  `ReadLine` reads a line the user edits.
- **Raw mode** (`SetMode(SF_CONSOLE_RAW)`) is for a program that draws the
  whole screen itself (`WriteAt`, `Draw`, `SetCursor`, `SetColor`) and
  takes every key through `ReadKey`. Key codes are in `sfos/keys.h`.

A cell holds one byte, drawn from the system font: code page 437. Beyond
ASCII it has lines and boxes, single and double, blocks, shades and
arrows. `sfos/chars.h` names them (`SF_BOX_H`, `SF_BOX2_TOP_LEFT`,
`SF_BLOCK_FULL`, ...), because in a UTF-8 source file "─" is three bytes,
not one.

Ctrl+C is an ordinary key: in `ReadLine` it ends the line with
`SF_ABORTED`, and in raw mode it is just a key. The system keeps some keys
for itself, and a program never sees them:

- Alt+F1..F9 switch screens.
- Ctrl+Alt+C ends the programs on the shown screen.
- Ctrl+Alt+Z pauses them and gives the keys to the console; pressing it
  again lets them go on.

A program gets keys only while it owns its screen's input. The console
gives the input to the program it starts. A program passes it on the same
way with `Start(..., SF_START_GIVE_INPUT, ...)`, and gets it back when
that program ends. Anyone else waiting in `ReadLine` or `ReadKey` waits for
its turn.

`SetTitle` puts the program's own words in the title bar, after its name.

### Files

A path always starts with a root:

| Root      | What it is |
|-----------|------------|
| `data:/`  | The program's own folder (`/files/<name>`), created on first start. |
| `tmp:/`   | Shared temporary files, emptied at every boot. `CreateUnique` makes a name nobody else uses. |
| `argN:`   | What argument N of the command line names. The console opens it for the program. |
| `disk:/`, `mount:/` | The whole boot volume and the other volumes: admin programs only. |

No path leads above its root. `Sys->Files->Open` gives an `SfFile`, and
paths opened from an `SfFile` are relative to it. The SfFile can read,
write, seek, list a folder (`ReadDir`) and describe itself (`GetInfo`).
`Sys->Files` also makes folders, deletes and renames.

### Programs and threads

`Sys->Process->Start` runs another program on the same screen and gives
a handle to `Wait` on. The flags choose:

- `SF_START_GIVE_INPUT`: hand it the keys.
- `SF_START_BACKGROUND`: run it on a hidden screen, with its output
  logged to `data:/console_<date>_<time>.log`.
- `SF_START_ADMIN`: give it the admin right; only for a caller that has
  it.

File arguments go in `ArgFiles` and reach the new program as its `argN:`
roots.

`Sys->Thread->Create` runs a function on a new thread, and `Join` waits
for it. The last thread to end, or `SfMain` returning, ends the program.
`Sys->Sync` gives mutexes and events to coordinate threads.

### Admin

A program started with the admin right (the console, `sudo <program>`)
gets `Sys->Admin` and the roots `disk:/` and `mount:/`. With them it can:

- list and end programs, and move them between screens (`fg`/`bg`);
- see the memory, the load of each CPU and what each program uses
  (`GetSystemInfo`, `GetProcessInfo`, revision 1.1);
- mount and unmount volumes;
- set the clock;
- restart and power off;
- read the kernel's reports (`lspci`, `dmesg`, ...).

The kernel checks the right on every call, so a program without it gets
`SF_ACCESS_DENIED` even if it gets past the missing table.

## Where things are

| Path                      | What is there |
|---------------------------|---------------|
| `include/sfos.h`, `include/sfos/` | The headers programs use. |
| `include/abi/`            | What the runtime and the kernel share: call numbers and statuses (`sfcall.h`), the runtime's image (`sdkimage.h`). Programs never make calls by number. |
| `runtime/`                | The code behind the tables. It is built into the kernel and mapped into every program at the same address, so programs do not link it. |
| `sfos.ld`                 | The linker script for programs. |

Example programs: `src/apps/hello.cpp` (the smallest),
`src/apps/taskmgr.cpp` (a full-screen program: raw mode, one `Draw` per
frame, keys read on a second thread), `src/sfos/cmd.cpp`
(the console: files, programs, admin), `src/apps/sdkcheck.cpp` (a check of
every table).
