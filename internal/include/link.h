/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _LINK_H_
#define _LINK_H_

#include <sys/elf.h>
#include <sys/types.h>

#ifndef _CADDR_T_DECLARED
typedef char *caddr_t;
#define _CADDR_T_DECLARED
#endif

/* Dl_serinfo origin flags */
#define LA_SER_ORIG 0x01
#define LA_SER_LIBPATH 0x02
#define LA_SER_RUNPATH 0x04
#define LA_SER_CONFIG 0x08
#define LA_SER_DEFAULT 0x40
#define LA_SER_SECURE 0x80

typedef struct link_map {
    caddr_t l_base;
    const char *l_name;
    const void *l_ld;
    struct link_map *l_next, *l_prev;
    caddr_t l_addr;
    const char *l_refname;
} Link_map;

struct r_debug {
    int r_version;
    struct link_map *r_map;
    void (*r_brk)(struct r_debug *, struct link_map *);
    enum { RT_CONSISTENT, RT_ADD, RT_DELETE } r_state;
    void *r_ldbase;
};

#define R_DEBUG_VERSION 1

struct dl_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

__BEGIN_DECLS

typedef int (*__dl_iterate_hdr_callback)(struct dl_phdr_info *, size_t, void *);
int dl_iterate_phdr(__dl_iterate_hdr_callback, void *);
int _ldso_addr_phdr(const void *, struct dl_phdr_info *);
int _ldso_get_stack_prot(void);
int _ldso_is_dlopened(void *);

__END_DECLS

#endif /* _LINK_H_ */
