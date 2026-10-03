# The SurfaceOS SDK

How to write a program for SurfaceOS, and every table of the SDK. The
headers (`include/sfos/*.h`) describe each function again, right above its
declaration.

- [A program](#a-program)
- [Tables](#tables)
- [Status](#status)
- [Overview](#overview): console, files, programs and threads, admin
- [Reference](#reference): [SfApp](#sfapp---app-sfosapph),
  [SfSystem](#sfsystem---sys-sfossystemh),
  [SfConsole](#sfconsole---sys-console-sfosconsoleh),
  [SfFiles](#sffiles---sys-files-sfosfilesh),
  [SfFile](#sffile---from-files-open-sfosfileh),
  [SfMemory](#sfmemory---sys-memory-sfosmemoryh),
  [SfTime](#sftime---sys-time-sfostimeh),
  [SfProcess](#sfprocess---sys-process-sfosprocessh),
  [SfThread](#sfthread---sys-thread-sfosthreadh),
  [SfSync](#sfsync-sfmutex-sfevent---sys-sync-sfossynch),
  [SfAdmin](#sfadmin---sys-admin-sfosadminh)
- [Where things are](#where-things-are)

## A program

A program is written in C or C++, the same way in both: one file
(`src/apps/<name>.c` or `src/apps/<name>.cpp`) or a folder of them
(`src/apps/<name>/`, `.c` and `.cpp` mixed). It includes one header and
implements `SfMain`:

```c
#include <sfos.h>

SfStatus SfMain(SfApp* App, SfSystem* Sys)
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

C and C++ see the same SDK:

- `sfos.h` declares `SfMain` with C linkage, so C++ needs no `extern "C"`
  of its own.
- The SDK has its own scalar types (`uint64_t`, `sint32_t`, `size_t`,
  ...), `NULL`, and in C `bool`, `true` and `false`. A program needs no
  other header.
- `SF_STATIC_ASSERT` checks layouts in both languages.

Every table, function, field and constant has a Doxygen comment
(`/// ...`) right above it, which the editor shows on hover and in
completion.

There is no libc and no start-up code. Programs are built freestanding
(C: `-std=c11 -ffreestanding -nostdlib`; C++: `-ffreestanding -nostdlib
-fno-exceptions -fno-rtti`) and linked with
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
| `Size`      | the size in bytes of the table as filled in                |

A table only grows at its end:

- A new function or field goes after the last one.
- Nothing is moved, removed or changed in meaning. A program built with an
  older SDK keeps working on a newer system.
- A program that wants something added later checks first that the system
  it runs on has it:

  ```cpp
  if (SF_HAS_FIELD(Sys, SfSystem, Admin) && Sys->Admin)
      ...
  ```

  `SF_HAS_FIELD` compares `Hdr.Size` with where the field ends.

Every header also pins its layout with `SF_STATIC_ASSERT`. A change that
would move a field breaks the build instead of every program built before
it. `sdkcheck` checks at run time that each table the system hands out
has the signature and size of the headers it was built with.

## Status

Every call returns an `SfStatus`: `SF_SUCCESS` (0), or an error with the
top bit set (`SF_ERROR(s)`). Results come back through pointer arguments.
The statuses are defined in `abi/sfcall.h`.

| Status                 | Meaning |
|------------------------|---------|
| `SF_SUCCESS`           | the call did what it was asked |
| `SF_UNSUPPORTED`       | no such call or operation (`ReadLine` in raw mode) |
| `SF_INVALID_PARAMETER` | a bad argument, such as a pointer the program does not own |
| `SF_NOT_FOUND`         | no such file, program or device |
| `SF_ACCESS_DENIED`     | not allowed: a path above its root, a call without the admin right |
| `SF_OUT_OF_RESOURCES`  | out of memory, handles or slots |
| `SF_ABORTED`           | cut short, for example by Ctrl+C in `ReadLine` |
| `SF_TIMEOUT`           | the time to wait ran out |
| `SF_END_OF_FILE`       | nothing more to read |
| `SF_BAD_HANDLE`        | a handle or id that refers to nothing of the kind |
| `SF_ALREADY_EXISTS`    | there is something by that name already |
| `SF_DEVICE_ERROR`      | the device or the volume on it failed |
| `SF_BUFFER_TOO_SMALL`  | the result does not fit; the size needed comes back |
| `SF_IN_USE`            | busy, for example a volume with files open |
| `SF_CRASHED`           | the program was ended by a CPU exception |

A program's own `SfMain` may return any status. `SF_ERROR_BIT | n` is
fine for a program-specific error.

## Overview

How the parts work together. Every function and type is listed in the
[Reference](#reference) below.

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

The clipboard is one for every program and screen. `SetClipboard` puts
bytes on it and `GetClipboard` takes them. In `ReadLine`
the user selects text on the screen with Ctrl+arrows, copies it with
Ctrl+C and types it with Ctrl+V.

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
roots. With `SF_START_OUTPUT` what it prints goes into a file, and with
`SF_START_INPUT` its `ReadLine` reads the lines of a file
instead of the keys, `SF_END_OF_FILE` after the last: the console's `>`
and `|`.

`Sys->Thread->Create` runs a function on a new thread, and `Join` waits
for it. The last thread to end, or `SfMain` returning, ends the program.
`Sys->Sync` gives mutexes and events to coordinate threads, and
`WaitAny` waits for whichever comes first of events,
programs and threads ending, and a key, with a timeout:

```cpp
SfWaitItem Items[2] = { { SF_WAIT_KEY, 0, NULL }, { SF_WAIT_EVENT, 0, Done } };
uint64_t Which;
if (Sys->Sync->WaitAny(Sys->Sync, 2, Items, 1000, &Which) == SF_SUCCESS && Which == 0)
    Con->ReadKey(Con, &Key);            // there now: no waiting
```

A program that draws a clock and reads keys needs no second thread for
it.

### Admin

A program started with the admin right (the console, `sudo <program>`)
gets `Sys->Admin` and the roots `disk:/` and `mount:/`. With them it can:

- list and end programs, and move them between screens (`fg`/`bg`);
- see the memory, the load of each CPU and what each program uses
  (`GetSystemInfo`, `GetProcessInfo`);
- mount and unmount volumes;
- set the clock;
- restart and power off;
- read the kernel's reports (`lspci`, `dmesg`, ...).

The kernel checks the right on every call, so a program without it gets
`SF_ACCESS_DENIED` even if it gets past the missing table.

## Reference

Every table of the SDK, with every function. Each function returns an
`SfStatus` and takes its own table as `This`, which the signatures below
leave out. A pointer argument marked *out* gets a result; *in/out* gives a
size in and gets one back.

### SfApp - `App`, `sfos/app.h`

The program itself, as the system describes it to `SfMain`.

| Field      | Type                 | What it holds |
|------------|----------------------|---------------|
| `Name`     | `const char*`        | The program's name (its file name in /apps). |
| `ArgCount` | `uint64_t`           | How many strings `Args` has. |
| `Args`     | `const char* const*` | The command line: `Args[0]` is the program as it was named, `Args[1]` and on its arguments. A path among them is opened by the console: `Files->Open("argN:")` opens what `Args[N]` names (a folder: `"argN:/file"`). |

### SfSystem - `Sys`, `sfos/system.h`

Everything a program can ask of the system. Each field is a table of its
own, below.

| Field     | Table       | What it is |
|-----------|-------------|------------|
| `Console` | `SfConsole` | The screen, the keyboard and the clipboard. |
| `Files`   | `SfFiles`   | The program's roots: `data:/`, `tmp:/`, `argN:`. |
| `Memory`  | `SfMemory`  | Pages and the heap. |
| `Time`    | `SfTime`    | The clock, the uptime and sleeping. |
| `Process` | `SfProcess` | Other programs and command lines. |
| `Thread`  | `SfThread`  | Threads of this program. |
| `Sync`    | `SfSync`    | Mutexes, events and `WaitAny`. |
| `Admin`   | `SfAdmin`   | Only with the admin right; null for any other program. |

### SfConsole - `Sys->Console`, `sfos/console.h`

The screen is a grid of cells, `Columns` x `Rows`, (0, 0) at the top
left, below the system's title bar. Keys reach the program that owns its
screen's input; anyone else waits in `ReadLine` or `ReadKey` for its turn.
Both of them also end with `SF_ABORTED` when another program on the screen
takes the input while they wait.

| Function | What it does |
|----------|--------------|
| `Print(const char* Text)` | Writes `Text` at the cursor in the colours of `SetColor`; wraps and scrolls. |
| `ReadLine(char* Buffer, uint64_t Size, uint64_t* Length)` | Reads one line typed by the user into `Buffer` (`Size` bytes with the NUL; no line break; a longer line is cut). *out* `Length`, may be null: the stored length. `SF_ABORTED` for Ctrl+C, `SF_END_OF_FILE` for Ctrl+D on an empty line, `SF_UNSUPPORTED` in raw mode. Up/Down bring back the last 16 lines; Ctrl+arrows select text on the screen, Ctrl+C then copies it, Ctrl+V types the clipboard. With `SF_START_INPUT` it reads lines of the input file instead. |
| `GetSize(uint32_t* Columns, uint32_t* Rows)` | *out*: the size of the screen. |
| `SetCursor(uint32_t Column, uint32_t Row, uint8_t Visible)` | Puts the cursor (where `Print` goes on) at the cell and shows or hides it. |
| `SetColor(uint8_t Foreground, uint8_t Background)` | The colours `Print` and `WriteAt` use from now on: `SF_COLOR_*`. |
| `WriteAt(uint32_t Column, uint32_t Row, const char* Text)` | Writes `Text` at the cell in the current colours; no wrapping (cut at the edge), the cursor stays. |
| `Draw(uint32_t Column, uint32_t Row, uint32_t Width, uint32_t Height, const SfCell* Cells)` | Fills a `Width` x `Height` rectangle with `Cells`, row by row; what falls outside is cut. |
| `ReadKey(SfKey* Key)` | Waits for a key; *out* `Key`. In line mode Ctrl+C gives `SF_ABORTED` instead. |
| `SetMode(uint64_t Mode)` | `SF_CONSOLE_LINE` or `SF_CONSOLE_RAW`; either switch clears the screen. |
| `SetTitle(const char* Text)` | The program's own words in the title bar, after its name; `""` clears them. |
| `Clear()` | Blanks the screen in the current colours, cursor to (0, 0). |
| `WaitInput()` | Waits until this program owns its screen's input again (the console does so while its program runs). `SF_ABORTED` when the program is being ended. |
| `SetHints(const char* Commands, const char* Names)` | Words `ReadLine` suggests while a word is typed, one a line: `Commands` for the first word (and after `\|`, `&`), `Names` for the others. Tab or Right takes the suggestion. Null or `""` for none. |
| `SetClipboard(const void* Data, uint64_t Size)` | Puts `Size` bytes on the clipboard, one for all programs; at most `SF_CLIPBOARD_SIZE` (else `SF_BUFFER_TOO_SMALL`); 0 empties it. |
| `GetClipboard(void* Buffer, uint64_t* Size)` | Copies the clipboard into `Buffer`. *in/out* `Size`: the buffer's size / the bytes it holds; `SF_BUFFER_TOO_SMALL` when they do not fit. |

| Type / constant | What it is |
|-----------------|------------|
| `SfCell { char Char; uint8_t Color; }` | One cell for `Draw`. `Char` is ASCII or a code page 437 character of `sfos/chars.h`; `Color` is `SF_CELL_COLOR(Fg, Bg)`. |
| `SfKey { uint16_t Code; uint8_t Mods; char Char; }` | A key from `ReadKey`: `SF_KEY_*`, `SF_MOD_SHIFT/CTRL/ALT`, and the character it types (0 for none; Ctrl+letter types 1..26). Codes are in `sfos/keys.h`. |
| `SF_COLOR_BLACK` .. `SF_COLOR_WHITE` (0..7), `SF_COLOR_BRIGHT` (8) | Colours; `SF_COLOR_BRIGHT \| c` is the bright form. |
| `SF_CONSOLE_LINE`, `SF_CONSOLE_RAW` | The modes: a scrolling text console, or a screen the program draws itself with keys only through `ReadKey`. |
| `SF_CLIPBOARD_SIZE` | 64 KiB, the most the clipboard holds. |

### SfFiles - `Sys->Files`, `sfos/files.h`

Every path starts with a root and never leads above it.

| Root     | What it is |
|----------|------------|
| `data:/` | The program's own folder (`/files/<name>`), created on first start. |
| `tmp:/`  | Shared temporary files, emptied at every boot; it cannot be listed. |
| `argN:`  | What argument N of the command line names. |
| `disk:/`, `mount:/` | The boot volume and the other volumes; admin programs only. |

| Function | What it does |
|----------|--------------|
| `Open(const char* Path, uint64_t Mode, SfFile** Out)` | Opens `Path` with `SF_FILE_*` `Mode`; *out* `Out`. `"data:/"` alone opens the root folder. `SF_INVALID_PARAMETER` without a root, `SF_NOT_FOUND` for an unknown root or a missing file, `SF_ACCESS_DENIED` above the root. |
| `CreateUnique(SfFile** Out, char* Path, uint64_t PathSize)` | Creates a new empty file in `tmp:/` under a name no other file has, open for reading and writing. *out* `Out`; `Path`, may be null, gets `"tmp:/..."`. |
| `CreateDirectory(const char* Path)` | Makes a folder; `SF_ALREADY_EXISTS` when the name is taken. |
| `Delete(const char* Path)` | Removes a file, or an empty folder (`SF_IN_USE` when it is not empty). |
| `Rename(const char* OldPath, const char* NewPath)` | Moves a file or folder; `NewPath` must not exist, both on one volume (else `SF_ACCESS_DENIED`). |

### SfFile - from `Files->Open`, `sfos/file.h`

An open file or folder. Paths opened from it are relative to it and never
lead above it.

| Function | What it does |
|----------|--------------|
| `Open(const char* Path, uint64_t Mode, SfFile** Out)` | Opens `Path` below this one; as `SfFiles` `Open`. `SF_ALREADY_EXISTS` with `SF_FILE_CREATE_NEW`. |
| `Close()` | Closes it; the pointer is invalid afterwards. |
| `Read(void* Buffer, uint64_t* Size)` | Reads at the position. *in/out* `Size`: how much to read / how much was read, 0 at the end. |
| `Write(const void* Buffer, uint64_t* Size)` | Writes at the position. *in/out* `Size`: how much to write / how much was written. |
| `GetPosition(uint64_t* Position)` | *out*: the offset of the next `Read` or `Write`. |
| `SetPosition(uint64_t Position)` | Moves it; past the end is allowed for writing. |
| `ReadDir(SfDirEntry* Entry)` | For a folder: *out* its next entry (without `.` and `..`); `SF_END_OF_FILE` after the last. `SetPosition(0)` starts again. |
| `GetInfo(SfDirEntry* Info)` | *out*: its size, whether it is a folder, when it changed; `Name` is left empty. |

| Type / constant | What it is |
|-----------------|------------|
| `SfDirEntry` | `Name[256]`, `Size` (0 for a folder), `Flags` (`SF_DIR_ENTRY_FOLDER`), `Modified` (`SfDateTime`). |
| `SF_FILE_READ`, `SF_FILE_WRITE` | Open for reading, for writing. |
| `SF_FILE_CREATE` | Create the file when it is missing. |
| `SF_FILE_CREATE_NEW` | Create it; `SF_ALREADY_EXISTS` when it is there. |
| `SF_FILE_TRUNCATE` | Empty it on open (with `SF_FILE_WRITE`). |

### SfMemory - `Sys->Memory`, `sfos/memory.h`

| Function | What it does |
|----------|--------------|
| `AllocatePages(uint64_t Count, void** Address)` | Maps `Count` zeroed pages of `SF_PAGE_SIZE` (4 KiB); *out* the first. `SF_OUT_OF_RESOURCES` when there is not that much memory. |
| `FreePages(void* Address, uint64_t Count)` | Gives back pages as `AllocatePages` returned them. |
| `Allocate(uint64_t Size, void** Buffer)` | `Size` bytes from the heap, zeroed and 16-byte aligned; *out* `Buffer`. |
| `Free(void* Buffer)` | Gives a block from `Allocate` back; `SF_INVALID_PARAMETER` for anything else. |

### SfTime - `Sys->Time`, `sfos/time.h`

| Function | What it does |
|----------|--------------|
| `GetTime(SfDateTime* Time)` | *out*: the date and time of the machine's clock. |
| `GetUptime(uint64_t* Milliseconds)` | *out*: the time since the system started, for measuring. |
| `Sleep(uint64_t Milliseconds)` | Does nothing that long; `SF_ABORTED` when cut short. |

`SfDateTime`: `Year`, `Month` (1..12), `Day` (1..31), `Hour` (0..23),
`Minute`, `Second` (0..59).

### SfProcess - `Sys->Process`, `sfos/process.h`

Every running program has a number, its Id.

| Function | What it does |
|----------|--------------|
| `GetId(uint64_t* Id)` | *out*: the calling program's number. |
| `GetArgs(uint64_t Id, char* Buffer, uint64_t* Size, uint64_t* Count)` | The command line of program `Id`, its strings one after another, each NUL-terminated. *in/out* `Size`; *out* `Count`, may be null. `SF_BUFFER_TOO_SMALL` (with `Size` set), `SF_NOT_FOUND`. |
| `Start(const char* Name, uint64_t ArgCount, const char* const* Args, SfFile* const* ArgFiles, uint64_t Flags, uint64_t* Handle)` | Starts program `Name` (from /apps, or a path with a root) on this screen; `Args[0]` becomes its `App->Args[1]`. `ArgFiles`, may be null, has `ArgCount` entries: a non-null one becomes its root `arg<i+1>:`. *out* `Handle` for `Wait`, may be null. Runs on when this program ends. `SF_NOT_FOUND` for no such program. |
| `Wait(uint64_t Handle, SfStatus* Status)` | Waits until that program has ended; *out* `Status`, may be null: what it returned, `SF_ABORTED` when it was ended, `SF_CRASHED` after a CPU exception. The handle is used up. |
| `IdOf(uint64_t Handle, uint64_t* Id)` | *out*: the number of the program behind `Handle`. |

| `Start` flag | What it does |
|--------------|--------------|
| `SF_START_GIVE_INPUT` | When this program owns the keys, the new one gets them until it ends. |
| `SF_START_BACKGROUND` | Runs it on a hidden screen of its own, what it prints logged to `console_<date>_<time>.log` in its data folder. |
| `SF_START_ADMIN` | With the admin right; only from a program that has it. |
| `SF_START_OUTPUT` | `ArgFiles` has one entry more, a file open for writing: what it prints in line mode goes there (the console's `>`). |
| `SF_START_INPUT` | `ArgFiles` has one entry more still, a file open for reading: `ReadLine` gives its lines (the console's `\|`). |

### SfThread - `Sys->Thread`, `sfos/thread.h`

`SfMain` runs on the first thread. A thread's code is
`SfStatus Entry(void* Arg)` (`SfThreadEntry`).

| Function | What it does |
|----------|--------------|
| `Create(SfThreadEntry Entry, void* Arg, uint64_t* Id)` | Starts `Entry(Arg)` on a new thread with a 256 KiB stack; *out* `Id` for `Join`. What `Entry` returns ends the thread. |
| `Exit(SfStatus Status)` | Ends the calling thread. The last thread to end ends the program with its status; `SfMain` returning ends every thread. |
| `Join(uint64_t Id, SfStatus* Status)` | Waits until thread `Id` has ended; *out* `Status`, may be null. Each thread is joined once; `SF_BAD_HANDLE` for no such thread. |

### SfSync, SfMutex, SfEvent - `Sys->Sync`, `sfos/sync.h`

| `SfSync` function | What it does |
|-------------------|--------------|
| `CreateMutex(SfMutex** Out)` | *out*: a new mutex, unlocked. |
| `CreateEvent(uint64_t Flags, SfEvent** Out)` | *out*: a new event, not set. With `SF_EVENT_AUTO_RESET` it resets itself each time a wait on it succeeds, so one `Set` lets one waiter through. |
| `WaitAny(uint64_t Count, const SfWaitItem* Items, uint64_t TimeoutMs, uint64_t* Index)` | Waits until one of at most `SF_WAIT_MAX_ITEMS` items is ready or `TimeoutMs` ran out (`SF_TIMEOUT`); *out* `Index`, may be null: the first ready one. Only that one auto-reset event is used up. No items: just sleeps. `SF_BAD_HANDLE` for an item that is not what its kind says. |

| `SfMutex` function | What it does |
|--------------------|--------------|
| `Lock()` | Waits until no other thread holds the mutex, then holds it. |
| `Unlock()` | Lets go of it; the thread that locked it unlocks it. |
| `Close()` | The mutex is not used any more; the pointer is invalid. |

| `SfEvent` function | What it does |
|--------------------|--------------|
| `Set()` | Raises it: every waiter goes on (auto-reset: one). |
| `Reset()` | Lowers it. |
| `Wait(uint64_t TimeoutMs)` | Waits until it is set; `SF_WAIT_FOREVER` for no limit, 0 just looks. `SF_TIMEOUT` when the time ran out. |
| `Close()` | The event is not used any more; the pointer is invalid. |

`SfWaitItem { uint64_t Kind; uint64_t Handle; SfEvent* Event; }` - the
kinds:

| Kind | Ready when |
|------|------------|
| `SF_WAIT_EVENT` | `Event` is set. |
| `SF_WAIT_PROCESS` | The program behind `Handle` (from `Start`) has ended; the handle stays for `Wait`. |
| `SF_WAIT_THREAD` | Thread `Handle` (from `Create`) has ended; it stays for `Join`. |
| `SF_WAIT_KEY` | A key is there: `ReadKey` gets it without waiting. |

### SfAdmin - `Sys->Admin`, `sfos/admin.h`

Only for a program started with the admin right (the console,
`sudo <program>`); null for any other. The kernel checks the right on
every call. Such a program also has the roots `disk:/` and `mount:/`.

| Function | What it does |
|----------|--------------|
| `ListProcesses(SfProcessInfo* Buffer, uint64_t* Count)` | The running programs. *in/out* `Count`: how many fit / how many there are; `SF_BUFFER_TOO_SMALL` when they do not fit. |
| `EndProcess(uint64_t Id)` | Ends program `Id` at once, as Ctrl+Alt+C would. `SF_NOT_FOUND`. |
| `Mount(const char* Device)` | Puts every partition of a disk (`"usb1"`: `mount:/usb1p1`, ...) or one partition (`"usb1p2"`) under `mount:/`. `SF_NOT_FOUND`, `SF_ALREADY_EXISTS`, `SF_DEVICE_ERROR` for a partition with no volume it can read. |
| `Unmount(const char* Device)` | Writes back and takes away what `Mount` put there. `SF_IN_USE` while a file on it is open, `SF_NOT_FOUND`. |
| `Restart()` | Writes every volume back and restarts the machine; comes back only on failure (`SF_DEVICE_ERROR`). |
| `ShutDown()` | The same, and powers off. |
| `Sync()` | Writes everything cached back to the disks. |
| `SetTime(const SfDateTime* Time)` | Sets the machine's clock. |
| `Report(const char* Topic, char* Buffer, uint64_t* Size)` | The kernel's text on `Topic`: `cpuid`, `lspci`, `lsusb`, `usbports`, `usbinfo <index>`, `lsblk`, `mount`, `acpi`, `meminfo`, `dmesg`. *in/out* `Size`; `SF_BUFFER_TOO_SMALL`, `SF_NOT_FOUND` for other topics. |
| `Foreground(uint64_t Id)` | Moves program `Id` (and what else is on its screen but the console) to the caller's screen and gives it the keys; a paused one goes on. `SF_IN_USE` when the caller's screen has other programs, `SF_ACCESS_DENIED` for a console or from the background. |
| `Background(uint64_t Id)` | The same, onto a hidden screen of its own, its output logged as with `SF_START_BACKGROUND`. |
| `GetSystemInfo(SfSystemInfo* Info)` | *out*: the memory and the CPUs now. A CPU's load over a time is how much its `CpuBusy` grew against its `CpuTotal`. |
| `GetProcessInfo(uint64_t Id, SfProcessStats* Info)` | *out*: what program `Id` uses. `SF_NOT_FOUND`. |

| Type | What it holds |
|------|---------------|
| `SfProcessInfo` | `Id`, `Screen` (1..9, 0 in the background), `Paused`, `Name[32]`. |
| `SfSystemInfo` | `MemoryTotal`, `MemoryFree` (bytes), `CpuCount`, `CpuName[48]`, `CpuBusy[]` and `CpuTotal[]` (ms per CPU, up to `SF_MAX_CPUS`). |
| `SfProcessStats` | `Id`, `ParentId` (0 when gone), `CpuTime` (ms), `Memory` (bytes), `Threads`, `Screen`, `Flags` (`SF_PROCESS_PAUSED`, `SF_PROCESS_ADMIN`), `Name[32]`. |

## Where things are

| Path                      | What is there |
|---------------------------|---------------|
| `include/sfos.h`, `include/sfos/` | The headers programs use. |
| `include/abi/`            | What the runtime and the kernel share: call numbers and statuses (`sfcall.h`), the runtime's image (`sdkimage.h`). Programs never make calls by number. |
| `runtime/`                | The code behind the tables. It is built into the kernel and mapped into every program at the same address, so programs do not link it. |
| `sfos.ld`                 | The linker script for programs. |

Example programs: `src/apps/hello.cpp` (the smallest), `src/apps/hello_c.c`
(the same SDK from C),
`src/apps/taskmgr.cpp` (a full-screen program: raw mode, one `Draw` per
frame, keys read on a second thread), `src/sfos/cmd.cpp`
(the console: files, programs, admin), `src/apps/sdkcheck.cpp` (a check of
every table).
