/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test for earc_decode.h against captured kernels (raw arm64 Image +
 * /proc/kallsyms; private LG files, never in this repository).
 *
 *   test_decode [--c5 DIR] [--g5 DIR] [--extra DIR]...
 *
 * --c5/--g5 assert the decoded layout equals the hand-derived C5 33.31.68 and
 * Glasshouse G5 33.31.68 values; the first given also runs the refusal tests,
 * and with both the C5/G5 difference test runs. --extra decodes and prints.
 * Run through test/run.sh, which checks each image's sha256 first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../earc_decode.h"

#define BASE 0xffffffc010088000ULL

static int failures;

struct kernel {
    const char *dir;
    unsigned char *image;
    size_t image_size;
    struct symbol { ed_u64 address; char name[96]; } *symbols;
    size_t count;
    ed_u64 *addresses;  /* sorted, unique, core kernel only */
    size_t address_count;
    struct earc_code code[EF_COUNT];
};

static const char *names[EF_COUNT] = {
#define EARC_NAME(name, analysed) #name,
    EARC_FUNCTIONS(EARC_NAME)
#undef EARC_NAME
};
static const int analysed[EF_COUNT] = {
#define EARC_ANALYSED(name, analysed) analysed,
    EARC_FUNCTIONS(EARC_ANALYSED)
#undef EARC_ANALYSED
};

static int compare_address(const void *a, const void *b) {
    ed_u64 x = *(const ed_u64 *)a, y = *(const ed_u64 *)b;
    return x < y ? -1 : x > y;
}

static void *read_file(const char *dir, const char *name, size_t *size) {
    char path[4096];
    FILE *file;
    unsigned char *data;
    long length;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length + 1);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) { fclose(file); free(data); return 0; }
    fclose(file);
    data[length] = 0;
    *size = (size_t)length;
    return data;
}

/* Same rules as the TV loader: exactly one kallsyms line of that name; size
 * up to the next greater core-kernel symbol. */
static int load_kernel(struct kernel *k, const char *dir) {
    size_t size, i, n;
    char *text = read_file(dir, "kallsyms.txt", &size), *line, *save = 0;
    memset(k, 0, sizeof(*k));
    k->dir = dir;
    k->image = read_file(dir, "kernel.bin", &k->image_size);
    if (!text || !k->image) { printf("SKIP %s: kernel.bin or kallsyms.txt missing\n", dir); return 0; }
    k->symbols = calloc(size / 16 + 1, sizeof(*k->symbols));
    k->addresses = calloc(size / 16 + 1, sizeof(*k->addresses));
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
        char type, name[96], module[64];
        unsigned long long address;
        int fields = sscanf(line, "%llx %c %95s %63s", &address, &type, name, module);
        if (fields < 3) continue;
        if (fields == 3) k->addresses[k->address_count++] = address;
        k->symbols[k->count].address = fields == 3 ? address : 0;
        snprintf(k->symbols[k->count].name, sizeof(k->symbols[k->count].name), "%s", name);
        k->count++;
    }
    free(text);
    qsort(k->addresses, k->address_count, sizeof(ed_u64), compare_address);
    for (i = n = 0; i < k->address_count; i++)
        if (!n || k->addresses[n - 1] != k->addresses[i]) k->addresses[n++] = k->addresses[i];
    k->address_count = n;
    for (i = 0; i < EF_COUNT; i++) {
        size_t j, found = 0;
        ed_u64 address = 0, end = 0;
        for (j = 0; j < k->count; j++)
            if (!strcmp(k->symbols[j].name, names[i])) { found++; address = k->symbols[j].address; }
        if (found != 1 || !address) { printf("FAIL %s: symbol %s found %zu times\n", dir, names[i], found); return -1; }
        k->code[i].address = address;
        if (!analysed[i]) continue;
        for (j = 0; j < k->address_count; j++)
            if (k->addresses[j] > address) { end = k->addresses[j]; break; }
        if (!end || address < BASE || end - BASE > k->image_size) { printf("FAIL %s: %s outside the image\n", dir, names[i]); return -1; }
        k->code[i].words = (const ed_u32 *)(k->image + (address - BASE));
        k->code[i].count = (ed_u32)((end - address) / 4);
    }
    return 1;
}

static void print_layout(const char *label, const struct earc_layout *l) {
    printf("  %s HAL_A=%#llx NONPCM=+%llu (%#llx) CS0=%#llx CS2=%#llx mode-select=%#llx\n", label,
           l->hal_a, l->nonpcm_offset, l->nonpcm, l->cs0, l->cs2, l->mode_select);
    printf("  %s HAL_B=%#llx rate=+%llu (%#llx) channels=%#llx acmod=+%llu (%#llx) control=+%llu (%#llx)\n", label,
           l->hal_b, l->rate_offset, l->rate, l->channel_count, l->acmod_offset, l->acmod, l->control_offset, l->control);
    printf("  %s BM mutex=%#llx clock table=%#llx stride=%#llx entry3=%#llx\n", label,
           l->bm_mutex, l->clock_table, l->clock_stride, l->clock_entry3);
}

static const char *decode(const struct earc_code *code, struct earc_layout *layout) {
    const char *error = earc_decode(code, layout);
    static char message[256];
    if (!error) return 0;
    snprintf(message, sizeof(message), "%s (at %#llx)", error, ed_error_pc);
    return message;
}

static void check(const char *label, const char *what, ed_u64 actual, ed_u64 expected) {
    if (actual == expected) return;
    printf("FAIL %s: %s = %#llx, expected %#llx\n", label, what, actual, expected);
    failures++;
}

struct oracle {
    const char *label;
    ed_u64 nonpcm, cs0, cs2, rate, channel_count, acmod, control, bm_mutex, clock_entry3;
    ed_u64 hal_a, hal_b, mode_select, clock_table, clock_stride;  /* 0: not part of the oracle */
};

/* C5 33.31.68: derived by hand from the disassembly, fingerprints matched live. */
static const struct oracle c5 = {
    "C5 33.31.68",
    0xffffffc01262a978ULL + 68, 0xffffffc01262a978ULL + 56, 0xffffffc01262a978ULL + 64,
    0xffffffc0122f8a58ULL + 104, 0xffffffc0122f8a58ULL, 0xffffffc0122f8a58ULL + 108, 0xffffffc0122f8a58ULL + 12,
    0xffffffc01262a0a8ULL, 0xffffffc0122f2bd0ULL,
    0xffffffc01262a978ULL, 0xffffffc0122f8a58ULL, 0xffffffc01262a978ULL, 0xffffffc0122f2ab0ULL, 0x60,
};
/* G5 33.31.69 (= Glasshouse's G5 33.31.68 image): Glasshouse's values. */
static const struct oracle g5 = {
    "G5 33.31.69",
    0xffffffc01264d5c4ULL, 0xffffffc01264d5b8ULL, 0xffffffc01264d5c0ULL,
    0xffffffc0122a7ec8ULL, 0xffffffc0122a7e68ULL, 0xffffffc0122a7eccULL, 0xffffffc0122a7e74ULL,
    0xffffffc01264cc98ULL, 0xffffffc0122a1fe0ULL,
    0, 0, 0, 0, 0x60,
};

static int run_oracle(const struct oracle *o, const struct kernel *k, struct earc_layout *l) {
    const char *error = decode(k->code, l);
    int before = failures;
    if (error) { printf("FAIL %s: decoder refused: %s\n", o->label, error); failures++; return 0; }
    print_layout(o->label, l);
    check(o->label, "NONPCM", l->nonpcm, o->nonpcm);
    check(o->label, "CS0", l->cs0, o->cs0);
    check(o->label, "CS2", l->cs2, o->cs2);
    check(o->label, "RATE", l->rate, o->rate);
    check(o->label, "CHANNEL_COUNT", l->channel_count, o->channel_count);
    check(o->label, "ACMOD", l->acmod, o->acmod);
    check(o->label, "CONTROL", l->control, o->control);
    check(o->label, "BM mutex", l->bm_mutex, o->bm_mutex);
    check(o->label, "clock entry 3", l->clock_entry3, o->clock_entry3);
    check(o->label, "clock stride", l->clock_stride, o->clock_stride);
    if (o->hal_a) check(o->label, "HAL_A", l->hal_a, o->hal_a);
    if (o->hal_b) check(o->label, "HAL_B", l->hal_b, o->hal_b);
    if (o->mode_select) check(o->label, "mode select", l->mode_select, o->mode_select);
    if (o->clock_table) check(o->label, "clock table", l->clock_table, o->clock_table);
    if (failures == before) printf("PASS %s: every derived value equals the oracle\n", o->label);
    return failures == before;
}

/* Copies one function, lets mutate change it, and requires a refusal whose
 * reason contains expected. */
static void refuse(const struct kernel *k, enum earc_function which, const char *what, const char *expected,
                   int (*mutate)(ed_u32 *words, ed_u32 count, const struct earc_layout *l), const struct earc_layout *l) {
    struct earc_code code[EF_COUNT];
    ed_u32 *copy;
    const char *error;
    struct earc_layout ignored;
    memcpy(code, k->code, sizeof(code));
    copy = malloc(4 * code[which].count);
    memcpy(copy, code[which].words, 4 * code[which].count);
    if (!mutate(copy, code[which].count, l)) {
        printf("FAIL refusal (%s): the instruction to mutate was not found\n", what);
        failures++;
        free(copy);
        return;
    }
    code[which].words = copy;
    error = decode(code, &ignored);
    if (error && strstr(error, expected)) printf("PASS refusal (%s): %s\n", what, error);
    else { printf("FAIL refusal (%s): got %s\n", what, error ? error : "acceptance"); failures++; }
    free(copy);
}

#define NOP 0xd503201fU

/* str wN, [xM, #nonpcm_offset] -> nop */
static int drop_nonpcm_store(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0xb9000000U && ((w[i] >> 10) & 0xfff) * 4 == l->nonpcm_offset) { w[i] = NOP; return 1; }
    return 0;
}
/* str wN, [xM, #rate_offset] -> the same store of register w2 (argument 2). */
static int store_wrong_rate(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0xb9000000U && ((w[i] >> 10) & 0xfff) * 4 == l->rate_offset) { w[i] = (w[i] & ~31U) | 2; return 1; }
    return 0;
}
/* The first BL -> nop (mutex_lock in bm_connect_block). */
static int drop_first_call(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xfc000000U) == 0x94000000U) { w[i] = NOP; return 1; }
    return 0;
}
/* The first mov x2, xN after a BL -> nop: x2 at the next call is then only a
 * value carried across the earlier call. */
static int drop_argument_copy(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i, called = 0;
    (void)l;
    for (i = 0; i < n; i++) {
        if ((w[i] & 0xfc000000U) == 0x94000000U) called = 1;
        else if (called && (w[i] & 0xffe0ffffU) == 0xaa0003e2U) { w[i] = NOP; return 1; }
    }
    return 0;
}
/* ldrb wN, [xM, #8] -> nop (the clock entry's ready flag). */
static int drop_entry_flag(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xfffffc00U) == 0x39402000U) { w[i] = NOP; return 1; }
    return 0;
}
/* orr wD, wN, #0x40000 -> #0x80000 (mute bit 18 -> 19). */
static int move_mute_bit(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xfffffc00U) == 0x320e0000U) { w[i] = (w[i] & 0x3ffU) | 0x320d0000U; return 1; }
    return 0;
}
/* The first RET -> br x16: an indirect branch (jump table). */
static int indirect_branch(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if (w[i] == 0xd65f03c0U) { w[i] = 0xd61f0200U; return 1; }
    return 0;
}
/* The first B -> a branch 1 MiB further on, outside the function. */
static int branch_out(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xfc000000U) == 0x14000000U) { w[i] = 0x14000000U | (0x100000U >> 2); return 1; }
    return 0;
}
/* and wD, wN, #0xfffffff8 -> #0xfffffff0: four mode bits instead of three. */
static int widen_mode_mask(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xfffffc00U) == 0x121d7000U) { w[i] = (w[i] & 0x3ffU) | 0x121c6c00U; return 1; }
    return 0;
}

static void refusal_tests(const struct kernel *k, const struct earc_layout *l) {
    struct earc_code code[EF_COUNT];
    struct earc_layout ignored;
    const char *error;
    refuse(k, EF_audio_hal_earc_set_output_format, "NONPCM store removed", "(NONPCM)", drop_nonpcm_store, l);
    refuse(k, EF_audio_hal_earc_set_output_format, "rate store takes argument 2", "(rate)", store_wrong_rate, l);
    refuse(k, EF_bm_connect_block, "mutex_lock call removed", "block-manager wrapper", drop_first_call, l);
    refuse(k, EF_bm_connect_block, "argument 2 only carried across mutex_lock", "pass the arguments through",
           drop_argument_copy, l);
    refuse(k, EF_aud_clock_update_output, "clock entry+8 read removed", "entry+8", drop_entry_flag, l);
    refuse(k, EF_audio_hal_earc_set_mute, "mute bit 18 moved to 19", "set_mute", move_mute_bit, l);
    refuse(k, EF_audio_hal_earc_set_audio_mode, "mode mask widened", "set_audio_mode", widen_mode_mask, l);
    refuse(k, EF_audio_hal_earc_set_channel, "RET turned into BR", "indirect branch", indirect_branch, l);
    refuse(k, EF_audio_hal_earc_set_audio_mode, "branch leaves the function", "branch leaves", branch_out, l);
    /* A wrong mutex_lock address from the loader. */
    memcpy(code, k->code, sizeof(code));
    code[EF_mutex_lock].address += 4;
    error = decode(code, &ignored);
    if (error && strstr(error, "mutex")) printf("PASS refusal (wrong mutex_lock address): %s\n", error);
    else { printf("FAIL refusal (wrong mutex_lock address): got %s\n", error ? error : "acceptance"); failures++; }
    /* A truncated function. */
    memcpy(code, k->code, sizeof(code));
    code[EF_audio_hal_earc_set_output_format].count = 8;
    error = decode(code, &ignored);
    if (error) printf("PASS refusal (set_output_format truncated to 8 instructions): %s\n", error);
    else { printf("FAIL refusal (truncated function): accepted\n"); failures++; }
}

/* The C5 and G5 layouts differ exactly where expected: every HAL_A and HAL_B
 * offset in use moved by 8 bytes except the mode/control word; nothing else. */
static void difference_test(const struct earc_layout *a, const struct earc_layout *b) {
    int before = failures;
    check("C5", "rate offset", a->rate_offset, 104);
    check("G5", "rate offset", b->rate_offset, 96);
    check("C5", "acmod offset", a->acmod_offset, 108);
    check("G5", "acmod offset", b->acmod_offset, 100);
    check("C5", "NONPCM offset", a->nonpcm_offset, 68);
    check("G5", "NONPCM offset", b->nonpcm_offset, 60);
    check("C5-G5", "control offset", a->control_offset, b->control_offset);
    check("C5-G5", "channel count offset", a->channel_count - a->hal_b, b->channel_count - b->hal_b);
    check("C5-G5", "mode select offset", a->mode_select - a->hal_a, b->mode_select - b->hal_a);
    check("C5-G5", "CS0 from NONPCM", a->nonpcm - a->cs0, b->nonpcm - b->cs0);
    check("C5-G5", "CS2 from NONPCM", a->nonpcm - a->cs2, b->nonpcm - b->cs2);
    check("C5-G5", "clock stride", a->clock_stride, b->clock_stride);
    if (failures == before)
        printf("PASS C5/G5 difference: rate +104/+96, acmod +108/+100, NONPCM +68/+60 (CS0/CS2 follow); control, channels, mode select, clock stride equal\n");
}

int main(int argc, char **argv) {
    struct kernel kernels[8];
    struct earc_layout layouts[8];
    int i, have_c5 = -1, have_g5 = -1, refused = 0, n = 0;
    for (i = 1; i + 1 < argc && n < 8; i += 2) {
        const char *option = argv[i], *dir = argv[i + 1];
        int loaded = load_kernel(&kernels[n], dir);
        if (loaded < 0) { failures++; continue; }
        if (!loaded) continue;
        if (!strcmp(option, "--c5") && run_oracle(&c5, &kernels[n], &layouts[n])) have_c5 = n;
        else if (!strcmp(option, "--g5") && run_oracle(&g5, &kernels[n], &layouts[n])) have_g5 = n;
        else if (!strcmp(option, "--extra")) {
            const char *error = decode(kernels[n].code, &layouts[n]);
            if (error) printf("INFO %s: refused: %s\n", dir, error);
            else { printf("INFO %s: decoded\n", dir); print_layout(dir, &layouts[n]); }
        }
        if (!refused && (have_c5 == n || have_g5 == n)) { refusal_tests(&kernels[n], &layouts[n]); refused = 1; }
        n++;
    }
    if (have_c5 >= 0 && have_g5 >= 0) difference_test(&layouts[have_c5], &layouts[have_g5]);
    else printf("SKIP C5/G5 difference test: needs both oracle kernels\n");
    if (!refused) printf("SKIP refusal tests: no oracle kernel decoded\n");
    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
