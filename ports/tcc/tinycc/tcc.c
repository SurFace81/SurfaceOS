/*
 *  TCC - Tiny C Compiler
 * 
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tcc.h"
#if ONE_SOURCE
# include "libtcc.c"
#endif

/* SurfaceOS: the options here are all that tcc_parse_args takes */
static const char help[] =
    "Tiny C Compiler "TCC_VERSION" for SurfaceOS\n"
    "Usage: tcc [options...] [-o outfile] [-c] infile(s)...\n"
    "       tcc [options...] folder    build every .c in it: folder/name.bin\n"
    "Options:\n"
    "  -o outfile  set output file name\n"
    "  -c          compile only - make an object file\n"
    "  -E          preprocess only\n"
    "  -Idir       add include path 'dir'\n"
    "  -Dsym[=val] define 'sym' with value 'val'\n"
    "  -Usym       undefine 'sym'\n"
    "  -Ldir       add library path 'dir'\n"
    "  -llib       link with library 'liblib.a'\n"
    "  -g          generate debug info\n"
    "  -w          disable all warnings\n"
    "  -Werror     stop at the first warning\n"
    "  -v          show version and the files tcc reads\n"
    "  -h          show this help\n"
    ;

static const char version[] =
    "tcc version "TCC_VERSION" (x86_64 SurfaceOS)\n";

static char *default_outputfile(TCCState *s, const char *first_file)
{
    char buf[1024];
    char *ext;
    const char *name = "a";

    if (first_file)
        name = tcc_basename(first_file);
    snprintf(buf, sizeof(buf), "%s", name);
    ext = tcc_fileextension(buf);
    if (s->output_type == TCC_OUTPUT_OBJ && *ext)
        strcpy(ext, ".o");
    else
        strcpy(buf, "a.out");
    return tcc_strdup(buf);
}

int main(int argc0, char **argv0)
{
    TCCState *s;
    int ret, opt, n = 0;
    const char *first_file;
    int argc; char **argv;
    SfFile *ppfp = NULL; /* the console */

redo:
    argc = argc0, argv = argv0;
    s = tcc_new();
    opt = tcc_parse_args(s, &argc, &argv, 1);

    if (n == 0) {
        if (opt == OPT_HELP)
            return sf_printf(NULL, help), 1;
        if (s->verbose)
            sf_printf(NULL, version);
        if (opt == OPT_V)
            return 0;

        n = s->nb_files;
        if (n == 0)
            tcc_error("no input files\n");

        if (s->output_type == TCC_OUTPUT_PREPROCESS) {
            if (s->outfile) {
                ppfp = sf_open(s->outfile, SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
                if (!ppfp)
                    tcc_error("could not write '%s'", s->outfile);
            }
        } else if (s->output_type == TCC_OUTPUT_OBJ) {
            if (s->nb_libraries)
                tcc_error("cannot specify libraries with -c");
            if (n > 1 && s->outfile)
                tcc_error("cannot specify output file with -c many files");
        }
    }

    if (s->output_type == 0)
        s->output_type = TCC_OUTPUT_EXE;
    tcc_set_output_type(s, s->output_type);
    s->ppfp = ppfp;

    /* compile or add each files or library */
    for (first_file = NULL, ret = 0;;) {
        struct filespec *f = s->files[s->nb_files - n];
        s->filetype = f->type;
        s->alacarte_link = f->alacarte;
        if (f->type == AFF_TYPE_LIB) {
            if (tcc_add_library_err(s, f->name) < 0)
                ret = 1;
        } else {
            if (1 == s->verbose)
                sf_printf(NULL, "-> %s\n", f->name);
            if (!first_file)
                first_file = f->name;
            if (tcc_add_file(s, f->name) < 0)
                ret = 1;
        }
        s->filetype = 0;
        s->alacarte_link = 1;
        if (--n == 0 || ret || s->output_type == TCC_OUTPUT_OBJ)
            break;
    }

    if (s->output_type != TCC_OUTPUT_PREPROCESS && 0 == ret) {
        if (!s->outfile)
            s->outfile = default_outputfile(s, first_file);
        if (tcc_output_file(s, s->outfile))
            ret = 1;
    }

    tcc_delete(s);
    if (ret == 0 && n)
        goto redo; /* compile more files with -c */
    if (ppfp)
        ppfp->Close(ppfp);
    return ret;
}
