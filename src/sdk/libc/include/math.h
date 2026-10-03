#ifndef SFOS_LIBC_MATH_H
#define SFOS_LIBC_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/// X * 2^Exp.
double ldexp(double X, int Exp);

#ifdef __cplusplus
}
#endif

#endif // SFOS_LIBC_MATH_H
