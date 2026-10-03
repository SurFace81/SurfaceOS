// libc: vsnprintf, snprintf, sprintf.

#include <stdio.h>

// Where the text goes: Buffer of Size bytes; Length counts all of it,
// also what did not fit.
typedef struct
{
    char*  Buffer;
    size_t Size;
    size_t Length;
} Output;

static void Put(Output* Out, char C)
{
    if (Out->Length + 1 < Out->Size)
        Out->Buffer[Out->Length] = C;
    Out->Length++;
}

static void PutRepeated(Output* Out, char C, int Count)
{
    for (; Count > 0; Count--)
        Put(Out, C);
}

// One field: Prefix (sign, 0x), Zeros zeros, Body of Length chars, padded
// with spaces to Width - on the right when Left.
static void PutField(Output* Out, const char* Prefix, int Zeros, const char* Body, int Length,
                     int Width, int Left)
{
    int PrefixLength = 0;
    while (Prefix[PrefixLength])
        PrefixLength++;
    int Pad = Width - PrefixLength - Zeros - Length;
    if (!Left)
        PutRepeated(Out, ' ', Pad);
    while (*Prefix)
        Put(Out, *Prefix++);
    PutRepeated(Out, '0', Zeros);
    for (int i = 0; i < Length; i++)
        Put(Out, Body[i]);
    if (Left)
        PutRepeated(Out, ' ', Pad);
}

// The digits of Value in Base, at the end of Buffer (24 chars: the 22
// octal digits of 2^64 fit). Returns the first.
static char* Digits(char* Buffer, unsigned long long Value, unsigned Base, int Upper)
{
    const char* Set = Upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char* P = Buffer + 24;
    do
    {
        *--P = Set[Value % Base];
        Value /= Base;
    } while (Value);
    return P;
}

// %f: Value with Precision digits after the point, into Buffer (Size
// bytes); a half rounds up (2.5 -> "3"). Returns the length.
static int FixedDigits(char* Buffer, int Size, long double Value, int Precision)
{
    int Length = 0;
    if (Value != Value || Value > 1e4900L)
    {
        const char* Word = Value != Value ? "nan" : "inf";
        for (; Length < 3; Length++)
            Buffer[Length] = Word[Length];
        return Length;
    }

    long double Half = 0.5L;
    for (int i = 0; i < Precision; i++)
        Half /= 10;
    Value += Half;
    long double Scale = 1;
    while (Scale * 10 <= Value)
        Scale *= 10;
    for (; Scale >= 1 && Length < Size; Scale /= 10)
    {
        int Digit = (int)(Value / Scale);
        Digit = Digit < 0 ? 0 : Digit > 9 ? 9 : Digit;
        Buffer[Length++] = (char)('0' + Digit);
        Value -= Digit * Scale;
    }
    if (Precision && Length < Size)
        Buffer[Length++] = '.';
    for (int i = 0; i < Precision && Length < Size; i++)
    {
        Value *= 10;
        int Digit = (int)Value;
        Digit = Digit < 0 ? 0 : Digit > 9 ? 9 : Digit;
        Buffer[Length++] = (char)('0' + Digit);
        Value -= Digit;
    }
    return Length;
}

int vsnprintf(char* Buffer, size_t Size, const char* Format, va_list Args)
{
    Output Out = { Buffer, Size, 0 };
    for (; *Format; Format++)
    {
        if (*Format != '%')
        {
            Put(&Out, *Format);
            continue;
        }

        int Left = 0, Zero = 0, Plus = 0, Space = 0, Alt = 0;
        for (;; Format++)
        {
            if (Format[1] == '-')
                Left = 1;
            else if (Format[1] == '0')
                Zero = 1;
            else if (Format[1] == '+')
                Plus = 1;
            else if (Format[1] == ' ')
                Space = 1;
            else if (Format[1] == '#')
                Alt = 1;
            else
                break;
        }
        Format++;

        int Width = 0, Precision = -1;
        if (*Format == '*')
        {
            Width = va_arg(Args, int);
            if (Width < 0)
            {
                Left = 1;
                Width = -Width;
            }
            Format++;
        }
        for (; *Format >= '0' && *Format <= '9'; Format++)
            Width = Width * 10 + (*Format - '0');
        if (*Format == '.')
        {
            Format++;
            Precision = 0;
            if (*Format == '*')
            {
                Precision = va_arg(Args, int);
                Format++;
            }
            for (; *Format >= '0' && *Format <= '9'; Format++)
                Precision = Precision * 10 + (*Format - '0');
        }

        // Size: 1 hh, 2 h, 4 int, 8 l ll z j t; LongDouble for L.
        int Bytes = 4, LongDouble = 0;
        if (*Format == 'h')
        {
            Bytes = Format[1] == 'h' ? 1 : 2;
            Format += Bytes == 1 ? 2 : 1;
        }
        else if (*Format == 'l' || *Format == 'z' || *Format == 'j' || *Format == 't')
        {
            Bytes = 8;
            Format += Format[0] == 'l' && Format[1] == 'l' ? 2 : 1;
        }
        else if (*Format == 'L')
        {
            LongDouble = 1;
            Format++;
        }

        char  Text[400];
        char* Body = Text;
        int   Length = 0;
        const char* Prefix = "";
        int   Zeros = 0;
        unsigned long long Value = 0;
        unsigned Base = 10;

        switch (*Format)
        {
        case 'd':
        case 'i':
        {
            long long Signed = Bytes == 8 ? va_arg(Args, long long) : va_arg(Args, int);
            if (Bytes == 1)
                Signed = (signed char)Signed;
            else if (Bytes == 2)
                Signed = (short)Signed;
            Value  = Signed < 0 ? 0 - (unsigned long long)Signed : (unsigned long long)Signed;
            Prefix = Signed < 0 ? "-" : Plus ? "+" : Space ? " " : "";
            goto Integer;
        }
        case 'o':
            Base = 8;
            goto Unsigned;
        case 'x':
        case 'X':
            Base = 16;
            goto Unsigned;
        case 'u':
        Unsigned:
            Value = Bytes == 8 ? va_arg(Args, unsigned long long) : va_arg(Args, unsigned);
            if (Bytes == 1)
                Value = (unsigned char)Value;
            else if (Bytes == 2)
                Value = (unsigned short)Value;
            if (Alt && Base == 16 && Value)
                Prefix = *Format == 'X' ? "0X" : "0x";
        Integer:
            if (Precision == 0 && Value == 0)
                Length = 0;             // "%.0d" of 0 prints nothing
            else
            {
                Body   = Digits(Text, Value, Base, *Format == 'X');
                Length = (int)(Text + 24 - Body);
            }
            if (Alt && Base == 8 && (Length == 0 || Body[0] != '0'))
                Zeros = 1;
            if (Precision > Length)
                Zeros = Precision - Length;
            else if (Zero && !Left && Precision < 0)
            {
                int PrefixLength = Prefix[0] ? (Prefix[1] ? 2 : 1) : 0;
                if (Width > PrefixLength + Zeros + Length)
                    Zeros = Width - PrefixLength - Length;
            }
            PutField(&Out, Prefix, Zeros, Body, Length, Width, Left);
            break;
        case 'p':
            Value  = (unsigned long long)(uintptr_t)va_arg(Args, void*);
            Body   = Digits(Text, Value, 16, 0);
            Length = (int)(Text + 24 - Body);
            PutField(&Out, "0x", 0, Body, Length, Width, Left);
            break;
        case 'c':
            Text[0] = (char)va_arg(Args, int);
            PutField(&Out, "", 0, Text, 1, Width, Left);
            break;
        case 's':
            Body = va_arg(Args, char*);
            if (!Body)
                Body = "(null)";
            while ((Precision < 0 || Length < Precision) && Body[Length])
                Length++;
            PutField(&Out, "", 0, Body, Length, Width, Left);
            break;
        case 'f':
        case 'F':
        {
            long double Real = LongDouble ? va_arg(Args, long double) : va_arg(Args, double);
            Prefix = Real < 0 ? "-" : Plus ? "+" : Space ? " " : "";
            Length = FixedDigits(Text, (int)sizeof(Text), Real < 0 ? -Real : Real,
                                 Precision < 0 ? 6 : Precision);
            if (Zero && !Left && Width > Length + (Prefix[0] != 0))
                Zeros = Width - Length - (Prefix[0] != 0);
            PutField(&Out, Prefix, Zeros, Text, Length, Width, Left);
            break;
        }
        case '%':
            Put(&Out, '%');
            break;
        default:                        // unknown: print it as it stands
            Put(&Out, '%');
            if (!*Format)
                Format--;               // a '%' at the very end
            else
                Put(&Out, *Format);
            break;
        }
    }
    if (Size)
        Buffer[Out.Length < Size ? Out.Length : Size - 1] = '\0';
    return (int)Out.Length;
}

int snprintf(char* Buffer, size_t Size, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    int Length = vsnprintf(Buffer, Size, Format, Args);
    va_end(Args);
    return Length;
}

int sprintf(char* Buffer, const char* Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    int Length = vsnprintf(Buffer, ~(size_t)0, Format, Args);
    va_end(Args);
    return Length;
}
