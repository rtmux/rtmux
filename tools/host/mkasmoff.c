/*
 * mkasmoff — generate assembly offset definitions from a compiled asmoff.o
 *
 * Reads an ELF64 object file compiled with the COMMENT/VALUE macros from
 * <mkasmoff.h> and produces a GAS-compatible assembly include file with .set
 * directives for each VALUE and comments for each COMMENT.
 *
 * Usage: mkasmoff [-f format] <infile> <outfile>
 *
 * Formats: gas (default), cpp
 *   - ELF64 only (no COFF, no OMF, no 32-bit ELF)
 *   - Little-endian only
 *   - GAS output by default (RISC-V), with cpp (#define) option
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

/* Minimal ELF64 definitions — avoid dependency on target headers */
#define EI_NIDENT 16
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define SHT_SYMTAB 2
#define SHN_UNDEF 0
#define STT_OBJECT 1

#define ELF64_ST_TYPE(i) ((i) & 0xf)

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* */

static Elf64_Ehdr ehdr;
static Elf64_Shdr *shdrs;
static char **shdata; /* loaded section data */
static unsigned shnum;

struct symbol {
    struct symbol *next;
    unsigned line;
    unsigned sect;
    uint64_t off;
    char name[1]; /* variable length */
};

static struct symbol *list;
static const char *comment_fmt;
static const char *value_fmt;

static void *load_section(FILE *f, unsigned i)
{
    if (shdata[i])
        return shdata[i];

    uint64_t off = shdrs[i].sh_offset;
    uint64_t size = shdrs[i].sh_size;

    shdata[i] = malloc(size);
    if (!shdata[i]) {
        fprintf(stderr, "No memory for section %u (%lu bytes)\n", i,
                (unsigned long) size);
        exit(1);
    }
    if (fseek(f, (long) off, SEEK_SET) != 0 ||
        fread(shdata[i], 1, size, f) != size) {
        fprintf(stderr, "Could not read section %u\n", i);
        exit(1);
    }
    return shdata[i];
}

/* The COMMENT and VALUE macros in mkasmoff.h produce symbols like:
 *   comment<LINE>_<text>       → comment string
 *   value<LINE>____<name>      → 4/8-byte value
 *
 * We parse the symbol name to extract the line number (for ordering) and the
 * user-visible name (after the prefix).
 */
static void add_symbol(unsigned sect, uint64_t off, const char *name)
{
    const char *p = NULL;
    unsigned line = 0;

#define COMMENT_PREFIX "comment"
#define VALUE_PREFIX "value"

    if ((p = strstr(name, COMMENT_PREFIX))) {
        p += sizeof(COMMENT_PREFIX) - 1;
        line = (unsigned) strtoul(p, (char **) &p, 10);
        p += 1; /* skip trailing '_' */
    } else if ((p = strstr(name, VALUE_PREFIX))) {
        p += sizeof(VALUE_PREFIX) - 1;
        line = (unsigned) strtoul(p, (char **) &p, 10);
        p += 4; /* skip trailing '____' */
    }

    if (p) {
        struct symbol *sym = malloc(sizeof(*sym) + strlen(p));
        struct symbol **owner;

        strcpy(sym->name, p);
        sym->sect = sect;
        sym->off = off;
        sym->line = line;

        /* Insert in order of increasing line number */
        for (owner = &list; *owner && (*owner)->line <= line;
             owner = &(*owner)->next)
            ;
        sym->next = *owner;
        *owner = sym;
    }
}

static void build_symbols(FILE *f)
{
    for (unsigned i = 0; i < shnum; i++) {
        if (shdrs[i].sh_type != SHT_SYMTAB)
            continue;

        Elf64_Sym *syms = load_section(f, i);
        char *strings = load_section(f, shdrs[i].sh_link);
        unsigned num = (unsigned) (shdrs[i].sh_size / sizeof(Elf64_Sym));

        for (unsigned j = 0; j < num; j++) {
            Elf64_Sym *sym = &syms[j];
            if (sym->st_shndx != SHN_UNDEF &&
                ELF64_ST_TYPE(sym->st_info) == STT_OBJECT) {
                add_symbol(sym->st_shndx, sym->st_value,
                           strings + sym->st_name);
            }
        }
    }
}

static void get_data(FILE *f,
                     struct symbol *sym,
                     unsigned size,
                     unsigned char *buf)
{
    char *p = (char *) load_section(f, sym->sect) + sym->off;

    if (size == 0)
        size = (unsigned) strlen(p) + 1;
    memcpy(buf, p, size);
}

struct format {
    const char *name;
    const char *comment;
    const char *value;
};

static const struct format formats[] = {
    {"gas", "# %s\n", ".set %s, 0x%lx\n"},
    {"cpp", "/* %s */\n", "#define %s 0x%lx\n"},
    {NULL, NULL, NULL}};

int main(int argc, char *argv[])
{
    int opt;
    const char *format_name = "gas";
    FILE *inf, *outf;
    unsigned i;

    while ((opt = getopt(argc, argv, "f:")) != -1) {
        switch (opt) {
        case 'f':
            format_name = optarg;
            break;
        default:
            fprintf(stderr,
                    "Usage: mkasmoff [-f gas|cpp] <infile> <outfile>\n");
            return 1;
        }
    }
    argv += optind;
    if (!argv[0] || !argv[1]) {
        fprintf(stderr, "Usage: mkasmoff [-f gas|cpp] <infile> <outfile>\n");
        return 1;
    }

    /* Find output format */
    for (i = 0; formats[i].name; i++) {
        if (!strcmp(formats[i].name, format_name))
            break;
    }
    if (!formats[i].name) {
        fprintf(stderr, "Unknown format '%s' (use gas or cpp)\n", format_name);
        return 1;
    }
    comment_fmt = formats[i].comment;
    value_fmt = formats[i].value;

    /* Open input */
    inf = fopen(argv[0], "rb");
    if (!inf) {
        fprintf(stderr, "Cannot open '%s': %s\n", argv[0], strerror(errno));
        return 1;
    }

    /* Read ELF header */
    if (fread(&ehdr, 1, sizeof(ehdr), inf) != sizeof(ehdr)) {
        fprintf(stderr, "Could not read ELF header\n");
        return 1;
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "'%s' is not an ELF file\n", argv[0]);
        return 1;
    }
    if (ehdr.e_ident[4] != ELFCLASS64) {
        fprintf(stderr, "Not a 64-bit ELF file\n");
        return 1;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        fprintf(stderr, "Not a little-endian ELF file\n");
        return 1;
    }
    if (ehdr.e_type != ET_REL) {
        fprintf(stderr, "Not a relocatable ELF file (type %d)\n", ehdr.e_type);
        return 1;
    }

    /* Read section headers */
    shnum = ehdr.e_shnum;
    shdrs = calloc(shnum, sizeof(Elf64_Shdr));
    shdata = calloc(shnum, sizeof(char *));
    if (!shdrs || !shdata) {
        fprintf(stderr, "No memory for section headers\n");
        return 1;
    }
    fseek(inf, (long) ehdr.e_shoff, SEEK_SET);
    if (fread(shdrs, sizeof(Elf64_Shdr), shnum, inf) != shnum) {
        fprintf(stderr, "Could not read section headers\n");
        return 1;
    }

    /* Build symbol list */
    build_symbols(inf);

    /* Open output */
    outf = fopen(argv[1], "w");
    if (!outf) {
        fprintf(stderr, "Cannot open '%s': %s\n", argv[1], strerror(errno));
        return 1;
    }

    /* Write header */
    fprintf(outf, comment_fmt,
            " This file was generated by mkasmoff — do not edit");
    char buf[256];
    snprintf(buf, sizeof(buf), "    mkasmoff -f%s %s %s", format_name, argv[0],
             argv[1]);
    fprintf(outf, comment_fmt, buf);
    fprintf(outf, "\n");

    /* Write symbols */
    while (list) {
        if (list->name[0] == '\0') {
            /* COMMENT — extract string from section data */
            unsigned char sbuf[256];
            get_data(inf, list, 0, sbuf);
            fprintf(outf, "\n");
            fprintf(outf, comment_fmt, (char *) sbuf);
        } else {
            /* VALUE — extract 8-byte little-endian value */
            unsigned char vbuf[8];
            get_data(inf, list, 8, vbuf);
            unsigned long val = (unsigned long) vbuf[0] +
                                ((unsigned long) vbuf[1] << 8) +
                                ((unsigned long) vbuf[2] << 16) +
                                ((unsigned long) vbuf[3] << 24) +
                                ((unsigned long) vbuf[4] << 32) +
                                ((unsigned long) vbuf[5] << 40) +
                                ((unsigned long) vbuf[6] << 48) +
                                ((unsigned long) vbuf[7] << 56);
            fprintf(outf, value_fmt, list->name, val);
        }
        list = list->next;
    }

    fclose(inf);
    fclose(outf);
    return 0;
}
