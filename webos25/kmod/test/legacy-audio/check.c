/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of target/legacy-audio against 4.4 captures: check DIR...
 * Each DIR holds kernel.bin and kallsyms.txt. Prints every function's measured shape and
 * size, then runs the module's own check and derivation and compares the derived data with
 * kallsyms. Fails if any capture is refused or derives a wrong address. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/hook.h"

static struct symbol { u64 address; char name[96]; } *symbols;
static size_t symbol_count;

static u64 kallsyms_lookup_name(const char *name)
{
    size_t i, found = 0;
    u64 address = 0;
    for (i = 0; i < symbol_count; i++)
        if (!strcmp(symbols[i].name, name)) { found++; address = symbols[i].address; }
    return found == 1 ? address : 0;
}

#include "../../target/legacy-audio/target.h"

static void *read_file(const char *dir, const char *name, size_t *size)
{
    char path[4096];
    FILE *file;
    unsigned char *data;
    long length;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(file = fopen(path, "rb"))) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length + 1);
    if (fread(data, 1, (size_t)length, file) != (size_t)length) { fclose(file); return 0; }
    fclose(file);
    data[length] = 0;
    *size = (size_t)length;
    return data;
}

static int run(const char *dir)
{
    size_t image_size, text_size, i, n = 0;
    unsigned char *image = read_file(dir, "kernel.bin", &image_size);
    char *text = read_file(dir, "kallsyms.txt", &text_size), *line, *save = 0;
    u64 base;
    struct earc_code fns[EF_COUNT];
    const char *error;
    if (!image || !text) { printf("FAIL %s: kernel.bin or kallsyms.txt missing\n", dir); return 1; }
    symbols = calloc(text_size / 16 + 1, sizeof *symbols);
    symbol_count = 0;
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
        unsigned long long address;
        char type, name[96];
        if (sscanf(line, "%llx %c %95s", &address, &type, name) != 3) continue;
        symbols[symbol_count].address = address;
        snprintf(symbols[symbol_count].name, sizeof symbols[0].name, "%s", name);
        symbol_count++;
    }
    base = kallsyms_lookup_name("_text");
    for (i = 0; i < EF_COUNT; i++) {
        struct earc_scan scan;
        u64 address = kallsyms_lookup_name(earc_functions[i].name), end = 0;
        for (n = 0; n < symbol_count; n++)
            if (symbols[n].address > address && (!end || symbols[n].address < end)) end = symbols[n].address;
        memset(&fns[i], 0, sizeof fns[i]);
        if (!address || !end || end - base > image_size) continue;
        fns[i].address = address;
        fns[i].words = (const u32 *)(image + (address - base));
        fns[i].count = (u32)((end - address) / 4);
        earc_scan(&fns[i], 0, 0, &scan);
        printf("  %-40s %4u 0x%08x%s\n", earc_functions[i].name, fns[i].count, scan.shape,
               fns[i].count != earc_functions[i].count || scan.shape != earc_functions[i].shape ? "  <- differs" : "");
        if (fns[i].count != earc_functions[i].count) fns[i].address = 0;
    }
    error = earc_check(fns);
    if (!error) error = earc_target_derive(fns);
    if (error) { printf("FAIL %s: refused (%s)\n", dir, error); return 1; }
    for (i = 0; i < sizeof earc_hooks / sizeof earc_hooks[0]; i++) {
        const struct earc_code *fn = &fns[earc_hooks[i].function];
        if (fn->words[earc_hooks[i].offset / 4] != earc_hooks[i].word) {
            printf("FAIL %s: hook %zu site word %08x\n", dir, i, fn->words[earc_hooks[i].offset / 4]);
            return 1;
        }
    }
    printf("PASS %s: eARC on %lx, main index %lx\n", dir, (u64)earc_on, (u64)earc_main_index);
    return 0;
}

int main(int argc, char **argv)
{
    int i, failures = 0;
    for (i = 1; i < argc; i++) {
        printf("%s\n", argv[i]);
        failures += run(argv[i]);
    }
    return failures != 0;
}
