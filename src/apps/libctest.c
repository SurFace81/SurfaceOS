// libctest: the SDK's libc (src/sdk/libc) in a program - strings, numbers
// from text, formatting, qsort, ldexp.
//
// Prints "libctest: N passed, M failed"; the exit status is M.

#include <sfos.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static SfConsole* Con;
static int Passed;
static int Failed;

static void Check(const char* Name, bool Ok)
{
    Con->Print(Con, Ok ? "  [ ok ] " : "  [FAIL] ");
    Con->Print(Con, Name);
    Con->Print(Con, "\n");
    if (Ok)
        Passed++;
    else
        Failed++;
}

// Formats with snprintf and compares with Expected.
static bool Formats(const char* Expected, const char* Format, ...)
{
    char Text[128];
    va_list Args;
    va_start(Args, Format);
    int Length = vsnprintf(Text, sizeof(Text), Format, Args);
    va_end(Args);
    return Length == (int)strlen(Expected) && !strcmp(Text, Expected);
}

static int CompareInts(const void* A, const void* B)
{
    return *(const int*)A - *(const int*)B;
}

static void CheckStrings(void)
{
    char Text[32];
    strcpy(Text, "hello");
    strcat(Text, ", world");
    Check("strcpy, strcat, strlen", !strcmp(Text, "hello, world") && strlen(Text) == 12);
    Check("strcmp, strncmp", strcmp("abc", "abd") < 0 && strcmp("b", "abc") > 0 &&
                             !strncmp("abc", "abd", 2) && strncmp("a", "ab", 5) < 0);
    Check("strchr, strrchr", strchr(Text, 'o') == Text + 4 && strrchr(Text, 'o') == Text + 8 &&
                             !strchr(Text, 'z') && strchr(Text, 0) == Text + 12);
    Check("strstr", strstr(Text, "world") == Text + 7 && strstr(Text, "") == Text &&
                    !strstr(Text, "worlds"));

    char Bytes[] = "0123456789";
    memmove(Bytes + 2, Bytes, 5);
    bool Ok = !strcmp(Bytes, "0101234789");
    memmove(Bytes, Bytes + 3, 5);
    Check("memmove both ways", Ok && !strcmp(Bytes, "1234734789"));
    memset(Bytes, 'x', 3);
    memcpy(Bytes + 3, "abc", 3);
    Check("memset, memcpy, memcmp", !memcmp(Bytes, "xxxabc", 6) && memcmp("a", "b", 1) < 0);
}

static void CheckNumbers(void)
{
    char* End = NULL;
    Check("strtol: decimal, sign, spaces", strtol("  -42x", &End, 10) == -42 && *End == 'x');
    Check("strtol base 0: 0x, 0", strtol("0x1F", NULL, 0) == 31 && strtol("0777", NULL, 0) == 511);
    Check("strtol: no number leaves End at the start",
          strtol("z", &End, 10) == 0 && *End == 'z');
    Check("strtol clamps", strtol("99999999999999999999", NULL, 10) == 0x7FFFFFFFFFFFFFFFL);
    Check("strtoull: the largest", strtoull("18446744073709551615", NULL, 10) == ~0ULL);
    Check("strtoul base 36", strtoul("zz", NULL, 36) == 35 * 36 + 35);

    Check("strtod: 1.5, -2.5e+3, .5", strtod("1.5", NULL) == 1.5 &&
                                      strtod("-2.5e+3", NULL) == -2500.0 &&
                                      strtod(".5", NULL) == 0.5);
    Check("strtod: 0.1 is the nearest double", strtod("0.1", NULL) == 0.1);
    Check("strtod: 1e400 is infinite, 1e-400 zero",
          strtod("1e400", NULL) > 1e308 && strtod("1e-400", NULL) == 0);
    Check("strtod: \"1e\" stops before the e", strtod("1e", &End) == 1.0 && *End == 'e');
    Check("strtof, strtold", strtof("0.25", NULL) == 0.25f && strtold("2.5", NULL) == 2.5L);

    Check("ldexp", ldexp(1.5, 4) == 24.0 && ldexp(1.0, -1074) > 0 &&
                   ldexp(1.0, -1075) == 0 && ldexp(1.0, 1024) > 1e308);

    int Items[100];
    for (int i = 0; i < 100; i++)
        Items[i] = (i * 37) % 101;
    qsort(Items, 100, sizeof(int), CompareInts);
    bool Sorted = true;
    for (int i = 1; i < 100; i++)
        Sorted = Sorted && Items[i - 1] <= Items[i];
    Check("qsort of 100 ints", Sorted);
}

static void CheckFormat(void)
{
    Check("%d %i %u", Formats("42 -7 3000000000", "%d %i %u", 42, -7, 3000000000u));
    Check("width, -, 0, +, space",
          Formats("   42|42   |00042|+42| 42", "%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42));
    Check("%x %X %#x %o", Formats("ff FF 0xff 10", "%x %X %#x %o", 255, 255, 255, 8));
    Check("ll, z, hh", Formats("-9223372036854775808 18446744073709551615 7 44",
                               "%lld %llu %zu %hhd", -9223372036854775807LL - 1,
                               18446744073709551615ULL, (size_t)7, 300));
    Check("precision of integers", Formats("005|     005|", "%.3d|%8.3d|", 5, 5));
    Check("%s with width and precision",
          Formats("abc|       abc|ab|xy", "%s|%10s|%.2s|%.*s", "abc", "abc", "abc", 2, "xyz"));
    Check("%c %p %%", Formats("a 0x1234 %", "%c %p %%", 'a', (void*)0x1234));
    Check("%f", Formats("1.500000 -2.25 3.142 99.9", "%f %.2f %.3f %0.1f", 1.5, -2.25,
                        3.14159, 99.94));

    char Small[5];
    Check("snprintf cuts short, returns the full length",
          snprintf(Small, sizeof(Small), "%s", "hello world") == 11 && !strcmp(Small, "hell"));
    char Text[32];
    Check("sprintf", sprintf(Text, "%s=%d", "x", 5) == 3 && !strcmp(Text, "x=5"));
}

SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    (void)App;
    Con = Sys->Console;
    Con->Print(Con, "libctest - the SDK's libc\n");
    CheckStrings();
    CheckNumbers();
    CheckFormat();

    char Line[64];
    snprintf(Line, sizeof(Line), "libctest: %d passed, %d failed\n", Passed, Failed);
    Con->Print(Con, Line);
    return (SfStatus)Failed;
}
