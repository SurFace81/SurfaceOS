// libc: numbers from text - strtol, strtoul, strtoull, strtod, strtof,
// strtold.

#include <stdlib.h>

static int IsSpace(char C)
{
    return C == ' ' || (C >= '\t' && C <= '\r');
}

static int DigitValue(char C)
{
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'z')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'Z')
        return C - 'A' + 10;
    return 99;
}

// The digits of an integer, as strtol describes. *Negative gets the sign;
// the value is clamped to Max (and *Over set) when larger.
static unsigned long long ReadInteger(const char* Text, char** End, int Base,
                                      unsigned long long Max, int* Negative, int* Over)
{
    const char* P = Text;
    *Negative = 0;
    *Over = 0;
    while (IsSpace(*P))
        P++;
    if (*P == '+' || *P == '-')
        *Negative = *P++ == '-';
    if ((Base == 0 || Base == 16) && P[0] == '0' && (P[1] == 'x' || P[1] == 'X') &&
        DigitValue(P[2]) < 16)
    {
        P += 2;
        Base = 16;
    }
    else if (Base == 0)
        Base = *P == '0' ? 8 : 10;

    const char*        Start = P;
    unsigned long long Value = 0;
    for (; DigitValue(*P) < Base; P++)
    {
        unsigned Digit = (unsigned)DigitValue(*P);
        if (Value > (Max - Digit) / (unsigned)Base)
            *Over = 1;
        else
            Value = Value * (unsigned)Base + Digit;
    }
    if (End)
        *End = (char*)(P == Start ? Text : P);
    return *Over ? Max : Value;
}

long strtol(const char* Text, char** End, int Base)
{
    int Negative, Over;
    // One more for the negative side: -9223372036854775808 fits.
    unsigned long long Value = ReadInteger(Text, End, Base, 0x8000000000000000ULL,
                                           &Negative, &Over);
    if (!Negative && Value > 0x7FFFFFFFFFFFFFFFULL)
        return 0x7FFFFFFFFFFFFFFFL;
    return Negative ? (long)(0 - Value) : (long)Value;
}

unsigned long long strtoull(const char* Text, char** End, int Base)
{
    int Negative, Over;
    unsigned long long Value = ReadInteger(Text, End, Base, ~0ULL, &Negative, &Over);
    return Negative && !Over ? 0 - Value : Value;
}

unsigned long strtoul(const char* Text, char** End, int Base)
{
    return (unsigned long)strtoull(Text, End, Base);
}

// 10^Exp, by squaring.
static long double PowerOfTen(int Exp)
{
    long double Result = 1, Square = 10;
    for (; Exp; Exp >>= 1, Square *= Square)
        if (Exp & 1)
            Result *= Square;
    return Result;
}

long double strtold(const char* Text, char** End)
{
    const char* P = Text;
    int Negative = 0;
    while (IsSpace(*P))
        P++;
    if (*P == '+' || *P == '-')
        Negative = *P++ == '-';

    // Up to 19 digits in Mantissa; any more only move the exponent.
    unsigned long long Mantissa = 0;
    int Digits = 0, Exp = 0, Any = 0;
    for (; *P >= '0' && *P <= '9'; P++, Any = 1)
    {
        if (Digits < 19)
        {
            Mantissa = Mantissa * 10 + (unsigned)(*P - '0');
            Digits += Mantissa != 0;
        }
        else
            Exp++;
    }
    if (*P == '.')
        for (P++; *P >= '0' && *P <= '9'; P++, Any = 1)
            if (Digits < 19)
            {
                Mantissa = Mantissa * 10 + (unsigned)(*P - '0');
                Digits += Mantissa != 0;
                Exp--;
            }
    if (!Any)
    {
        if (End)
            *End = (char*)Text;
        return 0;
    }
    if (*P == 'e' || *P == 'E')
    {
        const char* Q = P + 1;
        int Minus = 0, Value = 0;
        if (*Q == '+' || *Q == '-')
            Minus = *Q++ == '-';
        if (*Q >= '0' && *Q <= '9')
        {
            for (; *Q >= '0' && *Q <= '9'; Q++)
                if (Value < 100000)
                    Value = Value * 10 + (*Q - '0');
            Exp += Minus ? -Value : Value;
            P = Q;
        }
    }
    if (End)
        *End = (char*)P;

    long double Result = (long double)Mantissa;
    if (Result != 0)
    {
        // Two steps below the smallest long double's power, so that
        // 1e-4950 is not divided by an infinite 10^4950.
        if (Exp < -4900)
        {
            Result /= PowerOfTen(4900);
            Exp += 4900;
        }
        Result = Exp < 0 ? Result / PowerOfTen(-Exp) : Result * PowerOfTen(Exp);
    }
    return Negative ? -Result : Result;
}

double strtod(const char* Text, char** End)
{
    return (double)strtold(Text, End);
}

float strtof(const char* Text, char** End)
{
    return (float)strtold(Text, End);
}
