// libc: ldexp.

#include <math.h>
#include <abi/types.h>

double ldexp(double X, int Exp)
{
    union { double D; uint64_t U; } Bits = { X };
    if (X == 0 || ((Bits.U >> 52) & 0x7FF) == 0x7FF)
        return X;                       // zero, infinity, NaN
    // Beyond 3000 every double is infinite or zero all the same.
    if (Exp > 3000)
        Exp = 3000;
    if (Exp < -3000)
        Exp = -3000;
    // Steps that stay within a double's exponents (-1022..1023).
    for (; Exp > 1023; Exp -= 1023)
        X *= 0x1p1023;
    for (; Exp < -1022; Exp += 1022)
        X *= 0x1p-1022;
    Bits.U = (uint64_t)(Exp + 1023) << 52;
    return X * Bits.D;
}
