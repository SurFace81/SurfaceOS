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

## License

TinyCC is under the GNU Lesser General Public License 2.1 (`tinycc/COPYING`).
