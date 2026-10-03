# tcc - the Tiny C Compiler

A C compiler that runs in SurfaceOS and builds SurfaceOS programs: C99, its
own assembler and ELF linker, x86_64.

## Source

`tinycc/` is TinyCC 0.9.27 from the official release,
<https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2>
(SHA-256 `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`),
cut down to what x86_64 needs:

- the compiler: `tcc.c`, `tcctools.c`, `libtcc.c`, `tccpp.c`, `tccgen.c`,
  `tccelf.c`, `tccasm.c`, `tccrun.c`, `x86_64-gen.c`, `x86_64-link.c`,
  `i386-asm.c` and their headers;
- `include/`: the headers tcc gives the programs it builds;
- `lib/`: `libtcc1.c`, `va_list.c`, `alloca86_64.S` - the run-time helpers
  of the code tcc generates;
- `COPYING`, `README`, `VERSION`.

It was added unchanged in one commit; the changes for SurfaceOS come in
the commits after it, so `git diff` against that commit shows them all.

## On SurfaceOS

tcc is an SDK program like any other: `sfport.c` has its `SfMain`, which
calls tcc's `main` with the command line, and the few helpers through which
tcc reaches the system (`sfport.h`). The plain C functions come from the
SDK's libc. Built by the Makefile into `/apps/tcc`.

What it builds are SDK programs too: static ELF files that start in
`SfMain`, with `libtcc1.a` and the SDK's `libc.a` linked in. Its files are
in its data folder `/files/tcc` (`data:/` to tcc), put there by the
Makefile (`mkimg.py --tree`):

| Path | What is there |
|------|---------------|
| `include/` | tcc's `stdarg.h`, `stdbool.h`, `float.h`, `varargs.h`; the SDK's libc headers; `sfos.h` and the SDK headers |
| `lib/libtcc1.a` | `tinycc/lib`: what the code tcc makes calls (`va_arg`, `alloca`, long long conversions), built by gcc |
| `lib/libc.a` | the SDK's libc |

```
tcc hello.c -o hello.bin
```

What changed in `tinycc/`:

- files are `SfFile`s (`Sys->Files`): reading sources, objects and
  archives, writing the output and `-E`;
- a path with a root (`data:/x`) is used as it is; a path from the command
  line goes through the `argN:` root the console opened for it; any other
  path is below tcc's data folder `data:/`, where its `include/` and `lib/`
  are too. Lists of paths are split at `;`, since every root has a `:`;
- memory is the SDK heap (`Sys->Memory`, `Reallocate` for `tcc_realloc`);
- messages go to the console; an error ends tcc at once
  (`Process->Exit`) instead of `longjmp`;
- `__DATE__` and `__TIME__` from `Sys->Time`;
- programs: entry `SfMain` instead of `_start`, no `crt*.o`, `libtcc1.a`
  and `libc.a` from `data:/lib` instead of `-lc`, always static;
- `__SURFACEOS__` instead of `__unix__`; `size_t` is `unsigned long long`
  as in the SDK, so `include/stddef.h` is the SDK libc's (tcc's is gone);
- for the SDK headers: `_Static_assert`, `__builtin_offsetof`;
- two fixes for static programs, which 0.9.27 left with a GOT of zeros:
  calls go straight to the function instead of through a PLT, and
  `fill_got` runs before `tidy_section_headers` drops the relocations it
  reads;
- left out: `-run` and everything else that runs code in tcc's process,
  shared libraries, `-ar`, `-impdef`, `-m32`, `-MD` (`tcctools.c` is gone),
  stdin as `-`, environment variables.

## License

TinyCC is under the GNU Lesser General Public License 2.1 (`tinycc/COPYING`).
