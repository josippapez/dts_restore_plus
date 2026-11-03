/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test for earc_decode.h against captured kernels (raw arm64 Image +
 * /proc/kallsyms; private LG files, never in this repository).
 *
 *   test_decode [--kernel SHA256 DIR]...
 *
 * DIR holds kernel.bin and kallsyms.txt of one TV and SHA256 is the sha256 of
 * its kernel.bin (test/run.sh computes it). A kernel in the oracle table below
 * must decode to exactly the hand-derived values; the first oracle kernel of
 * each kernel generation (5.4, 6.12) also runs the refusal tests, and the
 * C5/G5 and 5.4/6.12 difference tests run when their kernels are given. Any
 * other kernel is decoded and printed without an oracle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../earc_decode.h"


static int failures;

struct kernel {
    const char *dir;
    unsigned char *image;
    size_t image_size;
    ed_u64 base;  /* link address of file offset 0: (_stext & ~0xfffff) + text_offset */
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
    for (i = 0, n = 0; i < k->count; i++)
        if (!strcmp(k->symbols[i].name, "_stext")) { n++; k->base = k->symbols[i].address; }
    if (n != 1 || k->image_size < 16) { printf("FAIL %s: _stext found %zu times\n", dir, n); return -1; }
    {
        ed_u64 text_offset = 0;
        for (i = 0; i < 8; i++) text_offset |= (ed_u64)k->image[8 + i] << (8 * i);
        k->base = (k->base & ~0xfffffULL) + text_offset;
    }
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
        if (!end || address < k->base || end - k->base > k->image_size) { printf("FAIL %s: %s outside the image\n", dir, names[i]); return -1; }
        k->code[i].words = (const ed_u32 *)(k->image + (address - k->base));
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
    printf("  %s clock mutex=+%#llx object rate=+%llu channels=%s%llu flag=+%llu callback=+%llu channel callback=%s%llu\n",
           label, l->clock_mutex_offset, l->clock_object_rate, l->clock_takes_channels ? "+" : "none ",
           l->clock_object_channels, l->clock_object_flag, l->clock_callback_offset,
           l->clock_takes_channels ? "+" : "none ", l->clock_channel_callback_offset);
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
    const char *sha256, *label;
    int generation, family;  /* 0: 5.4, 1: 6.12; 0: C5, 1: G5, 2: G2 */
    ed_u64 base;             /* expected link base of the image */
    ed_u64 nonpcm, cs0, cs2, rate, channel_count, acmod, control, bm_mutex, clock_entry3;
    ed_u64 hal_a, hal_b, mode_select, clock_table, clock_stride;  /* 0: not part of the oracle */
    ed_u64 mutex_offset, takes_channels, object_rate, object_channels, object_flag, callback, channel_callback;
};

#define BASE_54 0xffffffc010088000ULL
#define BASE_612 0xffffffc080000000ULL

/* 5.4 clock: mutex +0x38, rate +12, flag +16, callback +32, no channels. */
#define CLOCK_54 0x38, 0, 12, 0, 16, 32, 0
/* 6.12 clock: mutex +0x48, rate +8, channels +12, flag +16, callbacks +40/+48. */
#define CLOCK_612 0x48, 1, 8, 12, 16, 40, 48

/* A 6.12 image, from its hand-derived HAL_A, HAL_B, offsets, block-manager
 * mutex and clock table (stride 0x70, entry 3 = table + 0x150). */
#define ORACLE_612(sha, label, family, ha, hb, noff, roff, aoff, bm, table) \
    {sha, label, 1, family, BASE_612, (ha) + (noff), (ha) + (noff) - 12, (ha) + (noff) - 4, (hb) + (roff), (hb), \
     (hb) + (aoff), (hb) + 12, bm, (table) + 0x150, ha, hb, ha, table, 0x70, CLOCK_612}
#define C5_612(sha, label) ORACLE_612(sha, label, 0, 0xffffffc082eff578ULL, 0xffffffc082c4f600ULL, 0x44, 0x68, 0x6c, \
                                       0xffffffc082efebb0ULL, 0xffffffc082c4a468ULL)
#define G5_612(sha, label) ORACLE_612(sha, label, 1, 0xffffffc0829ff6e0ULL, 0xffffffc0827295b8ULL, 0x3c, 0x60, 0x64, \
                                       0xffffffc0829fed00ULL, 0xffffffc082724420ULL)

/* Hand-derived: C5 33.31.68 from the disassembly, fingerprints matched live;
 * G5 33.31.69 (= Glasshouse's G5 33.31.68 image) Glasshouse's values; the 6.12
 * images from each one's kallsyms (survey minus 0x10000) and disassembly. */
static const struct oracle oracles[] = {
    {"49859c6561689f7fb62d683eed77c9e0d415b8a62fbf6b374626a2a1d5e44f94", "C5 33.31.68", 0, 0, BASE_54,
     0xffffffc01262a978ULL + 68, 0xffffffc01262a978ULL + 56, 0xffffffc01262a978ULL + 64,
     0xffffffc0122f8a58ULL + 104, 0xffffffc0122f8a58ULL, 0xffffffc0122f8a58ULL + 108, 0xffffffc0122f8a58ULL + 12,
     0xffffffc01262a0a8ULL, 0xffffffc0122f2bd0ULL,
     0xffffffc01262a978ULL, 0xffffffc0122f8a58ULL, 0xffffffc01262a978ULL, 0xffffffc0122f2ab0ULL, 0x60, CLOCK_54},
    {"c219532590e1d691a459ef8c0faab95745d04d36b6cbee4a32685409f3e7460e", "G5 33.31.69", 0, 1, BASE_54,
     0xffffffc01264d5c4ULL, 0xffffffc01264d5b8ULL, 0xffffffc01264d5c0ULL,
     0xffffffc0122a7ec8ULL, 0xffffffc0122a7e68ULL, 0xffffffc0122a7eccULL, 0xffffffc0122a7e74ULL,
     0xffffffc01264cc98ULL, 0xffffffc0122a1fe0ULL,
     0, 0, 0, 0, 0x60, CLOCK_54},
    C5_612("53c3cb3d1f23a3da5e698df0a39848e850bbaa26692978e85845a12530775085", "C5 43.21.77"),
    C5_612("b19d1c3df9c5315a84314d9d817c080996ea037d967c68d76e11eba66da50cb2", "C5 43.21.73"),
    G5_612("44fe793cee4fe916a1896ba2f377fe38aac78137688d6137ad23253b5d8ded32", "G5 43.21.71"),
    G5_612("859189a41f6d3de71bfdd0bf47afaae257a539443e958c26f489d9e63c86dd5d", "G5 43.21.77"),
    ORACLE_612("ef593fd32dfa6f1b95efda3bd55a02b9a71485f2db078c49dd4cd53eb3d92e0a", "G2 43.21.74", 2,
               0xffffffc082ec9520ULL, 0xffffffc082c192d0ULL, 0x44, 0x68, 0x6c,
               0xffffffc082ec8bb0ULL, 0xffffffc082c14188ULL),
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
    check(o->label, "clock mutex offset", l->clock_mutex_offset, o->mutex_offset);
    check(o->label, "clock takes channels", l->clock_takes_channels, o->takes_channels);
    check(o->label, "clock object rate offset", l->clock_object_rate, o->object_rate);
    check(o->label, "clock object channels offset", l->clock_object_channels, o->object_channels);
    check(o->label, "clock object flag offset", l->clock_object_flag, o->object_flag);
    check(o->label, "clock callback offset", l->clock_callback_offset, o->callback);
    check(o->label, "clock channel callback offset", l->clock_channel_callback_offset, o->channel_callback);
    check(o->label, "image link base", k->base, o->base);
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

/* 6.12 only. sub xD, xN, xM (shifted register, no shift) -> nop: the index
 * scaling idx * 8 - idx * 1 loses its subtraction. */
static int drop_index_sub(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffe0fc00U) == 0xcb000000U) { w[i] = NOP; return 1; }
    return 0;
}
/* ldr xN, [xM, #channel callback] -> nop: the channel callback target is lost. */
static int drop_channel_callback_load(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0xf9400000U && ((w[i] >> 10) & 0xfff) * 8 == l->clock_channel_callback_offset) {
            w[i] = NOP;
            return 1;
        }
    return 0;
}
/* stp wA, wB, [xM, #object rate] -> nop, or -> the same pair at +16 (on top of the flag). */
static int drop_rate_pair(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0x29000000U && ((w[i] >> 15) & 0x7f) * 4 == l->clock_object_rate) { w[i] = NOP; return 1; }
    return 0;
}
static int move_rate_pair(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0x29000000U && ((w[i] >> 15) & 0x7f) * 4 == l->clock_object_rate) {
            w[i] = (w[i] & ~(0x7fU << 15)) | ((16U / 4) << 15);
            return 1;
        }
    return 0;
}

/* A self-consistent change of the index scaling is not a layout the decoder can
 * call wrong: the stride, the mutex and the entry loads all move together, so it
 * must follow the code (stride 0x80, entry 3 = table + 0x180) rather than a
 * remembered 0x70. This documents that the stride is derived, not assumed. */
static void follow_stride(const struct kernel *k, const struct earc_layout *l) {
    struct earc_code code[EF_COUNT];
    struct earc_layout changed;
    ed_u32 *copy = malloc(4 * k->code[EF_aud_clock_update_output].count);
    const char *error;
    memcpy(code, k->code, sizeof(code));
    memcpy(copy, code[EF_aud_clock_update_output].words, 4 * code[EF_aud_clock_update_output].count);
    if (!drop_index_sub(copy, code[EF_aud_clock_update_output].count, l)) {
        printf("FAIL derivation (index scaling subtraction removed): the instruction was not found\n");
        failures++;
        free(copy);
        return;
    }
    code[EF_aud_clock_update_output].words = copy;
    error = decode(code, &changed);
    if (!error && changed.clock_stride == 0x80 && changed.clock_entry3 == changed.clock_table + 3 * 0x80 &&
        changed.clock_table == l->clock_table)
        printf("PASS derivation (index scaling subtraction removed): stride follows the code, 0x80\n");
    else { printf("FAIL derivation (index scaling subtraction removed): %s\n", error ? error : "stride not 0x80"); failures++; }
    free(copy);
}

static void refusal_tests(const struct kernel *k, const struct earc_layout *l, int generation) {
    struct earc_code code[EF_COUNT];
    struct earc_layout ignored;
    const char *error;
    printf("-- refusal tests on %s (%s)\n", k->dir, generation ? "6.12" : "5.4");
    refuse(k, EF_audio_hal_earc_set_output_format, "NONPCM store removed", "(NONPCM)", drop_nonpcm_store, l);
    refuse(k, EF_audio_hal_earc_set_output_format, "rate store takes argument 2", "(rate)", store_wrong_rate, l);
    refuse(k, EF_bm_connect_block, "mutex_lock call removed", "block-manager wrapper", drop_first_call, l);
    refuse(k, EF_bm_connect_block, "argument 2 only carried across mutex_lock", "pass the arguments through",
           drop_argument_copy, l);
    refuse(k, EF_aud_clock_update_output, "clock entry+8 read removed", "entry+8", drop_entry_flag, l);
    refuse(k, EF_audio_hal_earc_set_mute, "mute bit 18 moved to 19", "set_mute", move_mute_bit, l);
    refuse(k, EF_audio_hal_earc_set_audio_mode, "mode mask widened", "set_audio_mode", widen_mode_mask, l);
    refuse(k, EF_audio_hal_earc_set_channel, "RET turned into BR", "indirect branch", indirect_branch, l);
    if (generation) {
        follow_stride(k, l);
        refuse(k, EF_aud_clock_update_output, "channel callback load removed", "control+Y", drop_channel_callback_load, l);
        refuse(k, EF_aud_clock_update_output, "rate/channels store removed", "rate (argument 1)", drop_rate_pair, l);
        refuse(k, EF_aud_clock_update_output, "rate/channels store moved onto the flag", "overlap", move_rate_pair, l);
    }
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

/* The C5 and G5 layouts of one generation differ exactly where expected: every
 * HAL_A and HAL_B offset in use moved by 8 bytes except the mode/control word;
 * nothing else. */
static void difference_test(const char *generation, const struct earc_layout *a, const struct earc_layout *b) {
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
    check("C5-G5", "clock mutex offset", a->clock_mutex_offset, b->clock_mutex_offset);
    check("C5-G5", "clock object rate offset", a->clock_object_rate, b->clock_object_rate);
    check("C5-G5", "clock callback offset", a->clock_callback_offset, b->clock_callback_offset);
    if (failures == before)
        printf("PASS %s C5/G5 difference: rate +104/+96, acmod +108/+100, NONPCM +68/+60 (CS0/CS2 follow); control, channels, mode select and the clock layout equal\n",
               generation);
}

/* 5.4 and 6.12 of one family differ exactly in the clock function's layout and
 * nowhere in the HAL: the decoder derived both from code. */
static void generation_test(const char *family, const struct earc_layout *a, const struct earc_layout *b) {
    int before = failures;
    check(family, "5.4 stride", a->clock_stride, 0x60);
    check(family, "6.12 stride", b->clock_stride, 0x70);
    check(family, "5.4 clock mutex offset", a->clock_mutex_offset, 0x38);
    check(family, "6.12 clock mutex offset", b->clock_mutex_offset, 0x48);
    check(family, "5.4 clock callback offset", a->clock_callback_offset, 32);
    check(family, "6.12 clock callback offset", b->clock_callback_offset, 40);
    check(family, "5.4 object rate offset", a->clock_object_rate, 12);
    check(family, "6.12 object rate offset", b->clock_object_rate, 8);
    check(family, "5.4 takes channels", a->clock_takes_channels, 0);
    check(family, "6.12 takes channels", b->clock_takes_channels, 1);
    check(family, "6.12 object channels offset", b->clock_object_channels, 12);
    check(family, "6.12 channel callback offset", b->clock_channel_callback_offset, 48);
    check(family, "object flag offset", a->clock_object_flag, b->clock_object_flag);
    check(family, "NONPCM offset", a->nonpcm_offset, b->nonpcm_offset);
    check(family, "rate offset", a->rate_offset, b->rate_offset);
    check(family, "acmod offset", a->acmod_offset, b->acmod_offset);
    check(family, "control offset", a->control_offset, b->control_offset);
    check(family, "channel count offset", a->channel_count - a->hal_b, b->channel_count - b->hal_b);
    check(family, "mode select offset", a->mode_select - a->hal_a, b->mode_select - b->hal_a);
    if (failures == before)
        printf("PASS %s 5.4/6.12 difference: stride 0x60/0x70, mutex +0x38/+0x48, callback +32/+40, object rate +12/+8, channels none/+12; HAL offsets equal\n",
               family);
}

int main(int argc, char **argv) {
    struct kernel kernels[16];
    struct earc_layout layouts[16];
    /* The first decoded oracle kernel of each (generation, family). */
    int first[2][3] = {{-1, -1, -1}, {-1, -1, -1}}, refused[2] = {0, 0};
    int i, n = 0;
    for (i = 1; i + 2 < argc && n < 16; i += 3) {
        const char *sha = argv[i + 1], *dir = argv[i + 2];
        const struct oracle *o = 0;
        size_t j;
        int loaded;
        if (strcmp(argv[i], "--kernel")) { printf("FAIL usage: test_decode [--kernel SHA256 DIR]...\n"); return 2; }
        loaded = load_kernel(&kernels[n], dir);
        if (loaded < 0) { failures++; continue; }
        if (!loaded) continue;
        for (j = 0; j < sizeof(oracles) / sizeof(oracles[0]); j++)
            if (!strcmp(oracles[j].sha256, sha)) o = &oracles[j];
        if (o) {
            if (run_oracle(o, &kernels[n], &layouts[n])) {
                if (first[o->generation][o->family] < 0) first[o->generation][o->family] = n;
                if (!refused[o->generation]) { refusal_tests(&kernels[n], &layouts[n], o->generation); refused[o->generation] = 1; }
            }
        } else {
            const char *error = decode(kernels[n].code, &layouts[n]);
            if (error) printf("INFO %s: refused: %s\n", dir, error);
            else { printf("INFO %s: decoded (no oracle)\n", dir); print_layout(dir, &layouts[n]); }
        }
        n++;
    }
    if (argc < 2 || (argc - 1) % 3) { printf("FAIL usage: test_decode [--kernel SHA256 DIR]...\n"); return 2; }
    for (i = 0; i < 2; i++) {
        if (first[i][0] >= 0 && first[i][1] >= 0) difference_test(i ? "6.12" : "5.4", &layouts[first[i][0]], &layouts[first[i][1]]);
        else printf("SKIP %s C5/G5 difference test: needs both oracle kernels\n", i ? "6.12" : "5.4");
        if (!refused[i]) printf("SKIP %s refusal tests: no oracle kernel decoded\n", i ? "6.12" : "5.4");
    }
    if (first[0][0] >= 0 && first[1][0] >= 0) generation_test("C5", &layouts[first[0][0]], &layouts[first[1][0]]);
    else printf("SKIP C5 5.4/6.12 difference test: needs both oracle kernels\n");
    if (first[0][1] >= 0 && first[1][1] >= 0) generation_test("G5", &layouts[first[0][1]], &layouts[first[1][1]]);
    else printf("SKIP G5 5.4/6.12 difference test: needs both oracle kernels\n");
    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
