/* The note that tells the kernel this is a SurfaceOS program (see
 * abi/sfcall.h). Linked into every program built against <sfos.h>; the
 * linker script (src/sdk/sfos.ld) puts it in a PT_NOTE segment. */

    .section .note.sfos, "a"
    .p2align 2
    .long 5                 /* namesz: "SFOS" and its NUL */
    .long 4                 /* descsz */
    .long 1                 /* type: SFOS_NOTE_ABI */
    .asciz "SFOS"
    .p2align 2
    .long 0x00010000        /* SF_SYSTEM_REVISION: 1.0 */
