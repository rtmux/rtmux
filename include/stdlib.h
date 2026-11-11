/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* stdlib.h Standard Library functions */
#ifndef _STDLIB_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <stddef.h>

#define _STDLIB_H_INCLUDED

#ifndef _STDLIB_H_DECLARED
#define _STDLIB_H_DECLARED

/* UNIX98 needs the exit status macros from <sys/wait.h> */
#ifndef __WAIT_H_INCLUDED
#include <sys/wait.h>
#endif

#ifndef NULL
#define NULL _NULL
#endif


#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* rand_r() range. RTMux provides only the reentrant rand_r(), not the
 * non-reentrant rand()/srand() with their hidden global seed.
 */
#define RAND_MAX 0x7fff
typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

__BEGIN_DECLS

extern void *calloc(size_t __n, size_t __size);
extern void free(void *__ptr);
extern void *malloc(size_t __size);
extern void *realloc(void *__ptr, size_t __size);

extern int posix_memalign(void **__memptr, size_t __alignment, size_t __size);

extern int rand_r(unsigned int *__seed);

extern void abort(void) __attribute__((__noreturn__));
extern int abs(int __j);
extern int atoi(const char *__nptr);
extern long int atol(const char *__nptr);
extern void *bsearch(const void *__key,
                     const void *__base,
                     size_t __nmemb,
                     size_t __size,
                     int (*__compar)(const void *__pkey, const void *__pbase));
extern div_t div(int __numer, int __denom);
extern void exit(int __status) __attribute__((__noreturn__));
extern char *getenv(const char *__name);
extern long int labs(long int __j);
extern ldiv_t ldiv(long int __numer, long int __denom);
extern void qsort(void *__base,
                  size_t __nmemb,
                  size_t __size,
                  int (*__compar)(const void *, const void *));
extern long int strtol(const char *__nptr, char **__endptr, int __base);
extern unsigned long int strtoul(const char *__nptr,
                                 char **__endptr,
                                 int __base);

extern int getsubopt(char **__optionp, char *const *__tokens, char **__valuep);

extern int setenv(const char *__name, const char *__value, int __rewrite);
extern int unsetenv(const char *__name);

extern int putenv(char *__string);

extern void _Exit(int __status);
extern long long strtoll(const char *__nptr, char **__endptr, int __base);
extern unsigned long long strtoull(const char *__nptr,
                                   char **__endptr,
                                   int __base);
extern long long atoll(const char *__nptr);

__END_DECLS
#endif
#ifdef __cplusplus
#ifdef _MATH_H_INCLUDED
inline int abs(int __j)
{
    return abs(__j);
};
#endif
#endif

#endif
