/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \file string.h
 *
 * Definitions for memory and string functions.
 */

#ifndef _STRING_H_
#define _STRING_H_

#include <sys/cdefs.h>
#include <sys/features.h>

#define __need_size_t
#define __need_NULL
#include <stddef.h>

__BEGIN_DECLS

void *memchr(const void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memcpy(void *__restrict, const void *__restrict, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
char *strcat(char *__restrict, const char *__restrict);
char *strchr(const char *, int);
int strcmp(const char *, const char *);
char *strcpy(char *__restrict, const char *__restrict);
size_t strcspn(const char *, const char *);
char *strerror(int);
int strerror_r(int, char *, size_t);
size_t strlen(const char *);
char *strncat(char *__restrict, const char *__restrict, size_t);
int strncmp(const char *, const char *, size_t);
char *strncpy(char *__restrict, const char *__restrict, size_t);
char *strpbrk(const char *, const char *);
char *strrchr(const char *, int);
size_t strspn(const char *, const char *);
char *strstr(const char *, const char *);
char *strtok(char *__restrict, const char *__restrict);
char *strtok_r(char *__restrict, const char *__restrict, char **__restrict);
void *memccpy(void *__restrict, const void *__restrict, int, size_t);
char *strdup(const char *) __malloc_like __result_use_check;
__END_DECLS

#if __SSP_FORTIFY_LEVEL > 0
#include <ssp/string.h>
#endif

#endif /* _STRING_H_ */
