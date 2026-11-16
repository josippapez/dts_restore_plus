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
#include "../earc_hdmi.h"
#include "../earc_probe.h"


static int failures;

struct kernel {
    const char *dir, *sha256;
    unsigned char *image;
    size_t image_size;
    ed_u64 base;  /* link address of file offset 0: (_stext & ~0xfffff) + text_offset */
    struct symbol { ed_u64 address; char name[96]; } *symbols;
    size_t count;
    ed_u64 *addresses;  /* sorted, unique, core kernel only */
    size_t address_count;
    struct earc_code code[EF_COUNT];
    ed_u64 stext, etext;              /* as in kallsyms */
    struct earc_code hdmi[HF_COUNT];  /* HDMI-set functions (optional) */
    ed_u64 hdmi_size[HF_COUNT];       /* their kallsyms sizes, valid or not */
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
static const char *hdmi_names[HF_COUNT] = {
#define HDMI_NAME(name, analysed) #name,
    EARC_HDMI_FUNCTIONS(HDMI_NAME)
#undef HDMI_NAME
};
static const int hdmi_analysed[HF_COUNT] = {
#define HDMI_ANALYSED(name, analysed) analysed,
    EARC_HDMI_FUNCTIONS(HDMI_ANALYSED)
#undef HDMI_ANALYSED
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
    k->stext = k->base;
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
        if (found != 1 || !address) { printf("REFUSE %s: symbol %s found %zu times\n", dir, names[i], found); return -2; }
        k->code[i].address = address;
        if (!analysed[i]) continue;
        for (j = 0; j < k->address_count; j++)
            if (k->addresses[j] > address) { end = k->addresses[j]; break; }
        if (!end || address < k->base || end - k->base > k->image_size) { printf("FAIL %s: %s outside the image\n", dir, names[i]); return -1; }
        k->code[i].words = (const ed_u32 *)(k->image + (address - k->base));
        k->code[i].count = (ed_u32)((end - address) / 4);
    }
    /* HDMI-set functions, built as the loader and the module build them: one
     * kallsyms line, the size up to the next greater core-kernel symbol, then
     * the module's own validity rule. A missing or invalid one stays zero. */
    for (i = 0; i < k->count; i++)
        if (!strcmp(k->symbols[i].name, "_etext")) k->etext = k->symbols[i].address;
    for (i = 0; i < HF_COUNT; i++) {
        size_t j, found = 0;
        ed_u64 address = 0, end = 0;
        for (j = 0; j < k->count; j++)
            if (!strcmp(k->symbols[j].name, hdmi_names[i])) { found++; address = k->symbols[j].address; }
        if (found != 1 || !address) continue;
        for (j = 0; j < k->address_count && hdmi_analysed[i]; j++)
            if (k->addresses[j] > address) { end = k->addresses[j]; break; }
        k->hdmi_size[i] = end ? end - address : 0;
        if (!earc_hdmi_valid(address, k->hdmi_size[i], hdmi_analysed[i], k->stext, k->etext)) continue;
        if (hdmi_analysed[i] && (address < k->base || end - k->base > k->image_size)) continue;
        k->hdmi[i].address = address;
        if (!hdmi_analysed[i]) continue;
        k->hdmi[i].words = (const ed_u32 *)(k->image + (address - k->base));
        k->hdmi[i].count = (ed_u32)(k->hdmi_size[i] / 4);
    }
    return 1;
}

/* A kernel-image pointer as it is after boot: the raw value, or, when that is
 * zero, the R_AARCH64_RELATIVE addend applied at boot (6.12 images). */
static ed_u64 hdmi_u64(const struct kernel *k, ed_u64 address) {
    ed_u64 value = 0, i;
    if (address < k->base || address - k->base + 8 > k->image_size) return 0;
    memcpy(&value, k->image + (address - k->base), 8);
    if (value) return value;
    for (i = 0; i + 24 <= k->image_size; i += 8) {
        ed_u64 off, info, add;
        memcpy(&off, k->image + i, 8);
        if (off != address) continue;
        memcpy(&info, k->image + i + 8, 8);
        if (info != 0x403) continue;
        memcpy(&add, k->image + i + 16, 8);
        return add;
    }
    return 0;
}

/* Consecutive table entries that point to a printable C string, as the module
 * counts them. */
static unsigned int hdmi_entries(const struct kernel *k, ed_u64 table) {
    unsigned int n = 0;
    if (!table) return 0;
    while (n < 32) {
        ed_u64 p = hdmi_u64(k, table + 8 * n), o;
        size_t e;
        if (p < k->etext || p - k->etext >= 0x4000000ULL || p < k->base || p - k->base >= k->image_size) break;
        o = p - k->base;
        for (e = o; e < k->image_size && e < o + 80 && k->image[e] >= 32 && k->image[e] < 127; e++) {}
        if (e == o || e >= k->image_size || k->image[e] != 0) break;
        n++;
    }
    return n;
}

/* The break-hook walk's memory read, on the image. */
static int image_read(void *context, ed_u64 address, unsigned int width, ed_u64 *value) {
    const struct kernel *k = context;
    ed_u32 word;
    if (address < k->base || address - k->base + width > k->image_size) return 0;
    if (width == 8) { *value = hdmi_u64(k, address); return 1; }
    memcpy(&word, k->image + (address - k->base), 4);
    *value = word;
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

/* ---- HDMI-set oracle (keyed by kernel sha256) ---- */
/* Each row was checked against an independent static derivation of the same
 * image (the review's), not taken from this decoder: every 5.4 value, and on
 * 6.12 everything but the name-table address, for which that derivation gave
 * the site offsets, the three instruction words and the six entries. The G2
 * 33.31.68 row is the tester's CS kernel and also equals the addresses compiled
 * into its modules. adec_base is the slot array (struct + 32); the independent
 * derivation frames it 32 lower with offsets 32 higher, the same addresses.
 * The G2 33.22.95 row (5.4.268-329.ptl4tv.2) was derived by hand from that
 * image's disassembly and kallsyms; there the slot array is the address formed
 * itself, and both ADEC functions bound the index with a signed `b.gt`.
 * ADEC 0 is the slot array itself (adec_base + 8 / +10 / +20): its open,
 * started and codec equal ADEC 1's minus the 96-byte stride in every row.
 * What one pattern fixes for a whole generation is asserted per generation:
 * mask +20, ADEC stride 96 / codec +20 / open +8 / started +10, break fn +16 /
 * immediate +24 / mask +26, HD bound 4, six name entries, and the sites: 5.4
 * shape 1 at +0x48 / +0x148 / +0x108, 6.12 shape 2 at +0x60 / +0x68 / +0xf0. */
struct hdmi_oracle {
    const char *sha256, *label;
    int generation;
    ed_u64 set_output, adec_base, adec0_open, adec0_started, adec0_codec, adec1_open, adec1_started, adec1_codec;
    ed_u64 break_list, break_fn, step_fn, patch_text, kprobe_reg, kprobe_unreg, cpu_lock, cpu_unlock;
    ed_u64 nonpcm_phys, cb, name_table;
};
static const struct hdmi_oracle hdmi_oracles[] = {
    {"49859c6561689f7fb62d683eed77c9e0d415b8a62fbf6b374626a2a1d5e44f94", "C5 33.31.68", 0,
     0xffffffc010cd5a60ULL, 0xffffffc01262a238ULL, 0xffffffc01262a240ULL, 0xffffffc01262a242ULL, 0xffffffc01262a24cULL,
     0xffffffc01262a2a0ULL, 0xffffffc01262a2a2ULL, 0xffffffc01262a2acULL,
     0xffffffc011a54090ULL, 0xffffffc01108b410ULL, 0xffffffc01108b300ULL, 0xffffffc01108b070ULL, 0xffffffc01017e7e0ULL, 0xffffffc01017dbc0ULL, 0xffffffc0100b8ed0ULL, 0xffffffc0100b9004ULL,
     0x262a9bcULL, 0xffffffc010cb3d44ULL, 0xffffffc0115493d8ULL},
    {"c219532590e1d691a459ef8c0faab95745d04d36b6cbee4a32685409f3e7460e", "G5 33.31.69", 0,
     0xffffffc010f5e550ULL, 0xffffffc01264ce48ULL, 0xffffffc01264ce50ULL, 0xffffffc01264ce52ULL, 0xffffffc01264ce5cULL,
     0xffffffc01264ceb0ULL, 0xffffffc01264ceb2ULL, 0xffffffc01264cebcULL,
     0xffffffc011f94090ULL, 0xffffffc011330bc0ULL, 0xffffffc011330ab0ULL, 0xffffffc011330820ULL, 0xffffffc01017e7e0ULL, 0xffffffc01017dbc0ULL, 0xffffffc0100b8ed0ULL, 0xffffffc0100b9004ULL,
     0x264d5c4ULL, 0xffffffc010f3c834ULL, 0xffffffc011a11698ULL},
    {"0606410b32b19fd10f77bb3bdd9b5c618380c717076638418169b7e7bbff80db", "G2 33.31.68 (CS)", 0,
     0xffffffc010cd44e0ULL, 0xffffffc012605218ULL, 0xffffffc012605220ULL, 0xffffffc012605222ULL, 0xffffffc01260522cULL,
     0xffffffc012605280ULL, 0xffffffc012605282ULL, 0xffffffc01260528cULL,
     0xffffffc011b04090ULL, 0xffffffc011088cd0ULL, 0xffffffc011088bc0ULL, 0xffffffc011088930ULL, 0xffffffc01017e7e0ULL, 0xffffffc01017dbc0ULL, 0xffffffc0100b8ed0ULL, 0xffffffc0100b9004ULL,
     0x260594cULL, 0xffffffc010cb2d54ULL, 0xffffffc0115ea500ULL},
    {"0192cf4f875affcbd505732ecc13d5fc67b38ae16f09d132ab21cbebdfa22503", "G2 33.22.95", 0,
     0xffffffc010cd8a70ULL, 0xffffffc0125f41e0ULL, 0xffffffc0125f41e8ULL, 0xffffffc0125f41eaULL, 0xffffffc0125f41f4ULL,
     0xffffffc0125f4248ULL, 0xffffffc0125f424aULL, 0xffffffc0125f4254ULL,
     0xffffffc011af4090ULL, 0xffffffc01108d090ULL, 0xffffffc01108cf80ULL, 0xffffffc01108ccf0ULL, 0xffffffc01017e740ULL, 0xffffffc01017db20ULL, 0xffffffc0100b8e90ULL, 0xffffffc0100b8fc4ULL,
     0x25f4934ULL, 0xffffffc010cb73c4ULL, 0xffffffc0115ea4b0ULL},
    {"53c3cb3d1f23a3da5e698df0a39848e850bbaa26692978e85845a12530775085", "C5 43.21.77", 1,
     0xffffffc080f5f4acULL, 0xffffffc082efedb8ULL, 0xffffffc082efedc0ULL, 0xffffffc082efedc2ULL, 0xffffffc082efedccULL,
     0xffffffc082efee20ULL, 0xffffffc082efee22ULL, 0xffffffc082efee2cULL,
     0xffffffc082284890ULL, 0xffffffc0813e3c94ULL, 0xffffffc0813e3bfcULL, 0xffffffc0813e3a4cULL, 0xffffffc08013c404ULL, 0xffffffc08013b760ULL, 0xffffffc08003f4c0ULL, 0xffffffc08003f9acULL,
     0x2eff5bcULL, 0xffffffc080f39bb8ULL, 0xffffffc08190e0d0ULL},
    {"b19d1c3df9c5315a84314d9d817c080996ea037d967c68d76e11eba66da50cb2", "C5 43.21.73", 1,
     0xffffffc080f5f4acULL, 0xffffffc082efedb8ULL, 0xffffffc082efedc0ULL, 0xffffffc082efedc2ULL, 0xffffffc082efedccULL,
     0xffffffc082efee20ULL, 0xffffffc082efee22ULL, 0xffffffc082efee2cULL,
     0xffffffc082284890ULL, 0xffffffc0813e1694ULL, 0xffffffc0813e15fcULL, 0xffffffc0813e144cULL, 0xffffffc08013c404ULL, 0xffffffc08013b760ULL, 0xffffffc08003f4c0ULL, 0xffffffc08003f9acULL,
     0x2eff5bcULL, 0xffffffc080f39bb8ULL, 0xffffffc08190e0d0ULL},
    {"44fe793cee4fe916a1896ba2f377fe38aac78137688d6137ad23253b5d8ded32", "G5 43.21.71", 1,
     0xffffffc080fb3f2cULL, 0xffffffc0829fef20ULL, 0xffffffc0829fef28ULL, 0xffffffc0829fef2aULL, 0xffffffc0829fef34ULL,
     0xffffffc0829fef88ULL, 0xffffffc0829fef8aULL, 0xffffffc0829fef94ULL,
     0xffffffc0824b4890ULL, 0xffffffc0814479d4ULL, 0xffffffc08144793cULL, 0xffffffc08144778cULL, 0xffffffc08013c404ULL, 0xffffffc08013b760ULL, 0xffffffc08003f4c0ULL, 0xffffffc08003f9acULL,
     0x29ff71cULL, 0xffffffc080f8e638ULL, 0xffffffc081b11e00ULL},
    {"859189a41f6d3de71bfdd0bf47afaae257a539443e958c26f489d9e63c86dd5d", "G5 43.21.77", 1,
     0xffffffc080fb3ff4ULL, 0xffffffc0829fef20ULL, 0xffffffc0829fef28ULL, 0xffffffc0829fef2aULL, 0xffffffc0829fef34ULL,
     0xffffffc0829fef88ULL, 0xffffffc0829fef8aULL, 0xffffffc0829fef94ULL,
     0xffffffc0824b4890ULL, 0xffffffc081447a94ULL, 0xffffffc0814479fcULL, 0xffffffc08144784cULL, 0xffffffc08013c404ULL, 0xffffffc08013b760ULL, 0xffffffc08003f4c0ULL, 0xffffffc08003f9acULL,
     0x29ff71cULL, 0xffffffc080f8e700ULL, 0xffffffc081b11e00ULL},
    {"ef593fd32dfa6f1b95efda3bd55a02b9a71485f2db078c49dd4cd53eb3d92e0a", "G2 43.21.74", 1,
     0xffffffc080f5ef70ULL, 0xffffffc082ec8d90ULL, 0xffffffc082ec8d98ULL, 0xffffffc082ec8d9aULL, 0xffffffc082ec8da4ULL,
     0xffffffc082ec8df8ULL, 0xffffffc082ec8dfaULL, 0xffffffc082ec8e04ULL,
     0xffffffc082324890ULL, 0xffffffc0813deed4ULL, 0xffffffc0813dee3cULL, 0xffffffc0813dec8cULL, 0xffffffc08013c404ULL, 0xffffffc08013b760ULL, 0xffffffc08003f4c0ULL, 0xffffffc08003f9acULL,
     0x2ec9564ULL, 0xffffffc080f3a168ULL, 0xffffffc0819af178ULL},
};

static ed_u64 kernel_symbol(const struct kernel *k, const char *name) {
    size_t i;
    for (i = 0; i < k->count; i++) if (!strcmp(k->symbols[i].name, name)) return k->symbols[i].address;
    return 0;
}

static void check_reason(const char *label, const char *what, const char *reason) {
    if (!reason) return;
    printf("FAIL %s: hdmi %s not available: %s\n", label, what, reason);
    failures++;
}

static int run_hdmi_oracle(const struct hdmi_oracle *o, const struct kernel *k, ed_u64 nonpcm, struct earc_hdmi_layout *h) {
    static const ed_u64 site[2][3] = {{0x48, 0x148, 0x108}, {0x60, 0x68, 0xf0}};
    struct earc_hdmi_walk walk;
    int before = failures;
    unsigned int i;
    earc_hdmi_decode(k->hdmi, nonpcm, k->stext & ~0xfffffULL, h);
    check(o->label, "hdmi gate state (stock)", h->gate_state, GATE_STOCK);
    check_reason(o->label, "set_output", h->set_output_reason);
    check(o->label, "hdmi set_output", h->set_output, o->set_output);
    check(o->label, "hdmi mask offset", h->mask_offset, 20);
    check_reason(o->label, "adec", h->adec_reason);
    check(o->label, "hdmi adec base", h->adec_base, o->adec_base);
    check(o->label, "hdmi adec stride", h->adec_stride, 96);
    check(o->label, "hdmi adec codec offset", h->adec_codec_off, 20);
    check(o->label, "hdmi adec open offset", h->adec_open_off, 8);
    check(o->label, "hdmi adec started offset", h->adec_started_off, 10);
    check(o->label, "hdmi adec0 open", h->adec0_open, o->adec0_open);
    check(o->label, "hdmi adec0 started", h->adec0_started, o->adec0_started);
    check(o->label, "hdmi adec0 codec", h->adec0_codec, o->adec0_codec);
    check(o->label, "hdmi adec1 open", h->adec1_open, o->adec1_open);
    check(o->label, "hdmi adec1 started", h->adec1_started, o->adec1_started);
    check(o->label, "hdmi adec1 codec", h->adec1_codec, o->adec1_codec);
    /* The slot the _set_output pre-handler reads for index 0 and 1. */
    for (i = 0; i < 2; i++) {
        struct earc_adec_slot slot = {0, 0, 0};
        int got = earc_adec_slot(h, i, &slot);
        check(o->label, i ? "hdmi slot for index 1: codec" : "hdmi slot for index 0: codec", slot.codec, i ? o->adec1_codec : o->adec0_codec);
        check(o->label, i ? "hdmi slot for index 1: open" : "hdmi slot for index 0: open", slot.open, i ? o->adec1_open : o->adec0_open);
        check(o->label, i ? "hdmi slot for index 1: started" : "hdmi slot for index 0: started", slot.started, i ? o->adec1_started : o->adec0_started);
        check(o->label, "hdmi slot given for index 0 and 1", (ed_u64)got, 1);
    }
    check_reason(o->label, "cb_output_status", h->cb_reason);
    check(o->label, "hdmi cb_output_status", h->cb_output_status, o->cb);
    check(o->label, "hdmi hd shape", (ed_u64)h->hd_shape, (ed_u64)o->generation + 1);
    check(o->label, "hdmi hd compare", h->hd_compare, o->cb + site[o->generation][0]);
    check(o->label, "hdmi hd compare word", h->hd_compare_word, 0x710010df);
    check(o->label, "hdmi hd compare bound", h->hd_compare_bound, 4);
    check(o->label, "hdmi hd supported branch", h->hd_supported_branch, o->cb + site[o->generation][1]);
    check(o->label, "hdmi hd supported word", h->hd_supported_word, 0xb9004666);
    check(o->label, "hdmi hd name load", h->hd_name_load, o->cb + site[o->generation][2]);
    check(o->label, "hdmi hd name word", h->hd_name_word, 0xf8647847);
    check(o->label, "hdmi hd output register", h->hd_output_reg, 19);
    check(o->label, "hdmi name table", h->name_table, o->name_table);
    check(o->label, "hdmi name table entries", hdmi_entries(k, h->name_table), 6);
    check_reason(o->label, "break hooks", h->break_reason);
    check(o->label, "hdmi break list head", h->break_list_head, o->break_list);
    check(o->label, "hdmi break fn offset", h->break_fn_offset, 16);
    check(o->label, "hdmi break immediate/mask derived", (ed_u64)h->break_fields_known, 1);
    check(o->label, "hdmi break immediate offset", h->break_imm_offset, 24);
    check(o->label, "hdmi break mask offset", h->break_mask_offset, 26);
    check(o->label, "hdmi kprobe handler", h->break_fn, o->break_fn);
    check(o->label, "hdmi kprobe step handler", h->step_fn, o->step_fn);
    check(o->label, "hdmi kretprobe handler (6.12 only)", h->kret_fn, kernel_symbol(k, "kretprobe_breakpoint_handler"));
    check(o->label, "hdmi kretprobe handler present", h->kret_fn != 0, (ed_u64)o->generation);
    check(o->label, "hdmi register_break", h->break_register, kernel_symbol(k, "register_kernel_break_hook"));
    check(o->label, "hdmi unregister_break", h->break_unregister, kernel_symbol(k, "unregister_kernel_break_hook"));
    check(o->label, "hdmi patch_text", h->patch_text, o->patch_text);
    check(o->label, "hdmi register_kprobe", h->kprobe_register, o->kprobe_reg);
    check(o->label, "hdmi unregister_kprobe", h->kprobe_unregister, o->kprobe_unreg);
    check(o->label, "hdmi cpus_read_lock", h->cpus_read_lock, o->cpu_lock);
    check(o->label, "hdmi cpus_read_unlock", h->cpus_read_unlock, o->cpu_unlock);
    check(o->label, "hdmi nonpcm physical", h->nonpcm_physical, o->nonpcm_phys);
    /* The module's walk, on the image: before boot the list is empty (the head
     * points at itself; on 6.12 through its relocation). */
    earc_hdmi_walk_list(h, k->etext, k->etext + 0x4000000ULL, image_read, (void *)k, &walk);
    check(o->label, "hdmi image walk status (complete)", (ed_u64)walk.status, HW_COMPLETE);
    check(o->label, "hdmi image walk nodes", walk.count, 0);
    if (failures == before) printf("PASS %s: HDMI set equals the oracle\n", o->label);
    return failures == before;
}

/* _set_output of every capture, with its kallsyms size, must pass the rule the
 * module applies before it scans a function (2544 bytes on the C5: above the
 * engine's 4 * ED_MAX_INSNS, which once rejected it on a TV only). */
static void hdmi_validity_test(const struct kernel *k) {
    ed_u64 address = kernel_symbol(k, "_set_output"), size = k->hdmi_size[HF__set_output];
    if (earc_hdmi_valid(address, size, 1, k->stext, k->etext) && k->hdmi[HF__set_output].count == size / 4)
        printf("PASS %s: hdmi _set_output (%llu bytes) passes the loader validity rule\n", k->dir, size);
    else { printf("FAIL %s: hdmi _set_output (%llu bytes at %#llx) fails the loader validity rule\n", k->dir, size, address); failures++; }
}

static void hdmi_rule_test(void) {
    const ed_u64 stext = 0x1000, etext = 0x100000, a = 0x2000;
    if (earc_hdmi_valid(a, 2544, 1, stext, etext) && earc_hdmi_valid(a, 4 * HF_MAX_INSNS, 1, stext, etext) &&
        earc_hdmi_valid(a, 0, 0, stext, etext) && !earc_hdmi_valid(a + 2, 8, 1, stext, etext) &&
        !earc_hdmi_valid(stext - 4, 8, 1, stext, etext) && !earc_hdmi_valid(etext, 8, 1, stext, etext) &&
        !earc_hdmi_valid(a, 0, 1, stext, etext) && !earc_hdmi_valid(a, 6, 1, stext, etext) &&
        !earc_hdmi_valid(etext - 4, 8, 1, stext, etext) && !earc_hdmi_valid(a, 4 * HF_MAX_INSNS + 4, 1, stext, etext) &&
        !earc_hdmi_valid(0, 0, 0, stext, etext))
        printf("PASS hdmi validity rule: takes 2544 bytes and the tracker's maximum; rejects misaligned, outside text, empty, odd, past _etext, oversized\n");
    else { printf("FAIL hdmi validity rule\n"); failures++; }
}

/* ---- the break-hook walk, on a list built in test memory ---- */
#define FAKE_BASE 0xffffffc012000000ULL
struct fake_memory { ed_u64 words[256]; int stray; };
static int fake_read(void *context, ed_u64 address, unsigned int width, ed_u64 *value) {
    struct fake_memory *m = context;
    if (address < FAKE_BASE || address - FAKE_BASE + width > sizeof(m->words)) { m->stray = 1; return 0; }
    *value = 0;
    memcpy(value, (const char *)m->words + (address - FAKE_BASE), width);
    return 1;
}
/* Node i: 32 bytes at FAKE_BASE + 64 + 32 * i, as struct break_hook {next, prev, fn, imm:16, mask:16}. */
static ed_u64 fake_node(struct fake_memory *m, unsigned int i, ed_u64 next, ed_u64 fn, ed_u32 word) {
    m->words[8 + 4 * i] = next;
    m->words[8 + 4 * i + 2] = fn;
    m->words[8 + 4 * i + 3] = word;
    return FAKE_BASE + 64 + 32 * i;
}
static void walk_case(const char *what, struct fake_memory *m, int status, unsigned int count, ed_u64 stop, int saw_break, int saw_step) {
    struct earc_hdmi_layout l;
    struct earc_hdmi_walk w;
    memset(&l, 0, sizeof(l));
    l.break_list_head = FAKE_BASE;
    l.break_fn_offset = 16;
    l.break_word_offset = 24;
    l.break_fn = 0xffffffc011000100ULL;
    l.step_fn = 0xffffffc011000200ULL;
    m->stray = 0;
    earc_hdmi_walk_list(&l, FAKE_BASE, FAKE_BASE + sizeof(m->words), fake_read, m, &w);
    if (w.status == status && w.count == count && (status == HW_COMPLETE || w.stop == stop) && w.saw_break == saw_break &&
        w.saw_step == saw_step && !m->stray)
        printf("PASS hdmi walk (%s): status %d, %u node(s)\n", what, w.status, w.count);
    else {
        printf("FAIL hdmi walk (%s): status %d count %u stop %#llx break %d step %d stray read %d\n", what, w.status, w.count,
               w.stop, w.saw_break, w.saw_step, m->stray);
        failures++;
    }
}
static void hdmi_walk_tests(void) {
    static struct fake_memory m;
    const ed_u64 break_fn = 0xffffffc011000100ULL, step_fn = 0xffffffc011000200ULL, module = 0xffffffc008a01000ULL;
    unsigned int i;
    memset(&m, 0, sizeof(m));
    m.words[0] = FAKE_BASE;
    walk_case("empty list", &m, HW_COMPLETE, 0, 0, 0, 0);
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, fake_node(&m, 1, FAKE_BASE, step_fn, 6), break_fn, 4);
    walk_case("complete, both kprobe handlers linked", &m, HW_COMPLETE, 2, 0, 1, 1);
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, module, 0xffffffc011000300ULL, 0x800);
    walk_case("leaves the image for a module's node, which is not read", &m, HW_OUTSIDE, 1, module, 0, 0);
    memset(&m, 0, sizeof(m));
    for (i = 0; i < 40; i++) fake_node(&m, i, i == 39 ? FAKE_BASE : FAKE_BASE + 64 + 32 * (i + 1), 0xffffffc011000300ULL, 0);
    m.words[0] = FAKE_BASE + 64;
    walk_case("longer than the 32-node cap", &m, HW_CAPPED, 32, FAKE_BASE + 64 + 32 * 32, 0, 0);
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, FAKE_BASE + 64 + 32 + 4, break_fn, 4);
    walk_case("misaligned next pointer", &m, HW_MISALIGNED, 1, FAKE_BASE + 64 + 32 + 4, 1, 0);
}

/* ---- stage-a break-hook decision, through the walk the module runs ---- */
#define DEC_BREAK_FN 0xffffffc011000100ULL
#define DEC_STEP_FN 0xffffffc011000200ULL
static void decide_case(const char *what, struct fake_memory *m, int fields_known, enum earc_hook_decision expect) {
    struct earc_hdmi_layout l;
    struct earc_hdmi_walk w;
    enum earc_hook_decision got;
    memset(&l, 0, sizeof(l));
    l.break_list_head = FAKE_BASE;
    l.break_fn_offset = 16;
    l.break_word_offset = 24;
    l.break_fn = DEC_BREAK_FN;
    l.step_fn = DEC_STEP_FN;
    m->stray = 0;
    earc_hdmi_walk_list(&l, FAKE_BASE, FAKE_BASE + sizeof(m->words), fake_read, m, &w);
    got = earc_hook_decide(&w, fields_known);
    if (got == expect && !m->stray) printf("PASS hook decision (%s): %d\n", what, got);
    else { printf("FAIL hook decision (%s): got %d expected %d stray %d\n", what, got, expect, m->stray); failures++; }
}
static void hook_decision_tests(void) {
    static struct fake_memory m;
    const ed_u64 other = 0xffffffc011000300ULL;
    /* Empty list, complete: nothing to answer BRK #4/#6 -> register. */
    memset(&m, 0, sizeof(m));
    m.words[0] = FAKE_BASE;
    decide_case("empty list -> register", &m, 1, EHD_REGISTER);
    /* Both kprobe handlers already linked -> already. */
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, fake_node(&m, 1, FAKE_BASE, DEC_STEP_FN, 6), DEC_BREAK_FN, 4);
    decide_case("both handlers present -> already", &m, 1, EHD_ALREADY);
    /* A foreign node that answers BRK #4 (imm 4, mask 0) -> conflict. */
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, FAKE_BASE, other, 4);
    decide_case("foreign node answers #4 -> conflict", &m, 1, EHD_CONFLICT);
    /* A foreign node whose mask makes it answer #6 (imm 6, mask 0) -> conflict. */
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, FAKE_BASE, other, 6);
    decide_case("foreign node answers #6 -> conflict", &m, 1, EHD_CONFLICT);
    /* A BUG() hook (imm 0x800, mask 0) does not answer #4 or #6 -> register. */
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, FAKE_BASE, other, 0x800);
    decide_case("bug hook only -> register", &m, 1, EHD_REGISTER);
    /* Same list, but the immediate/mask fields were not derived -> refuse. */
    decide_case("node present, fields unknown -> conflict", &m, 0, EHD_CONFLICT);
    /* Walk leaves the image (a module node) -> incomplete. */
    memset(&m, 0, sizeof(m));
    m.words[0] = fake_node(&m, 0, 0xffffffc008a01000ULL, other, 0);
    decide_case("walk leaves image -> incomplete", &m, 1, EHD_INCOMPLETE);
}

/* ---- gate "restore only what we wrote" ---- */
static void gate_tests(void) {
    int ok = earc_gate_restore_word(EARC_GATE_OPEN_W0) == EARC_GATE_STOCK_W0 &&
             earc_gate_restore_word(EARC_GATE_STOCK_W0) == 0 &&
             earc_gate_restore_word(0xdeadbeefU) == 0;
    if (ok) printf("PASS gate restore: restores only our word, leaves a stock or foreign word\n");
    else { printf("FAIL gate restore\n"); failures++; }
}

/* ---- per-mode refusal from the derived layout ---- */
static void good_hdmi_layout(struct earc_hdmi_layout *h) {
    memset(h, 0, sizeof(*h));
    h->gate_state = GATE_STOCK;
    h->break_reason = h->adec_reason = h->set_output_reason = h->cb_reason = 0;
    h->break_fn = 0xffffffc011000100ULL;
    h->step_fn = 0xffffffc011000200ULL;
    h->break_fields_known = 1;
    h->break_register = 0xffffffc011000600ULL;
    h->break_unregister = 0xffffffc011000700ULL;
    h->patch_text = 0xffffffc011000300ULL;
    h->cpus_read_lock = 0xffffffc011000400ULL;
    h->cpus_read_unlock = 0xffffffc011000500ULL;
    h->set_output_first_brk = 0;
}
static void refusal_case(const char *what, const struct earc_hdmi_layout *h, int should_refuse) {
    const char *r = earc_mode_refusal(h);
    if (!r == !should_refuse) printf("PASS refusal (%s): %s\n", what, r ? r : "accepted");
    else { printf("FAIL refusal (%s): got %s\n", what, r ? r : "accepted"); failures++; }
}
static void mode_refusal_tests(void) {
    struct earc_hdmi_layout h;
    good_hdmi_layout(&h);
    refusal_case("all derived", &h, 0);
    good_hdmi_layout(&h); h.break_reason = "break not derived";
    refusal_case("break not derived", &h, 1);
    good_hdmi_layout(&h); h.break_fields_known = 0;
    refusal_case("break fields unknown", &h, 1);
    good_hdmi_layout(&h); h.break_register = 0;
    refusal_case("register_kernel_break_hook missing", &h, 1);
    good_hdmi_layout(&h); h.break_unregister = 0;
    refusal_case("unregister_kernel_break_hook missing", &h, 1);
    good_hdmi_layout(&h); h.gate_state = GATE_BREAKPOINT;
    refusal_case("gate holds a breakpoint", &h, 1);
    good_hdmi_layout(&h); h.patch_text = 0;
    refusal_case("patch_text missing", &h, 1);
    good_hdmi_layout(&h); h.set_output_reason = "set_output not derived";
    refusal_case("set_output not derived", &h, 1);
    good_hdmi_layout(&h); h.set_output_first_brk = 1;
    refusal_case("set_output entry holds a breakpoint", &h, 1);
    good_hdmi_layout(&h); h.adec_reason = "adec not derived";
    refusal_case("adec not derived", &h, 1);
}

/* ---- DTS-HD probes ---- */
static void hd_case(const char *label, const char *what, int ok) {
    if (ok) printf("PASS %s: %s\n", label, what);
    else { printf("FAIL %s: %s\n", label, what); failures++; }
}

/* On every kernel given: the two sites and their registers derive, the session
 * accepts the image, and each decision acts and does not act with that
 * kernel's own bound and table size. A kernel that already accepts and names
 * codec 7 (C3/G3, G4) is accepted without probes; one whose stock gate already
 * returns 1 (C3/G3, G4, 2026) without a gate patch. */
static void hd_probe_test(const struct kernel *k, ed_u64 nonpcm) {
    struct earc_hdmi_layout h;
    unsigned int entries, codec, output, index, name, i;
    const char *reason, *last;
    ed_u64 bound, last_pointer;
    int keep = 1;
    earc_hdmi_decode(k->hdmi, nonpcm, k->stext & ~0xfffffULL, &h);
    entries = hdmi_entries(k, h.name_table);
    reason = earc_hd_refusal(&h, entries);
    if (!reason) reason = earc_mode_refusal(&h);
    if (reason) { printf("FAIL %s: the session refuses a stock image: %s\n", k->dir, reason); failures++; return; }
    if (h.gate_state == GATE_SWITCHED)
        printf("  %s stock DTS gate already returns 1: accepted without a patch\n", k->dir);
    if (earc_hd_native(&h, entries)) {
        printf("  %s hd native: cmp w%u,#%llu, %u names; no probes placed\n", k->dir, earc_hd_codec_reg(&h),
               h.hd_compare_bound, entries);
        hd_case(k->dir, "stock compare already accepts codec 7 and LG's own name load stays inside the table",
                h.hd_compare_word == (0x7100001fU | ((ed_u32)h.hd_compare_bound << 10) | (earc_hd_codec_reg(&h) << 5)) &&
                EARC_HD_EVENT_CODEC <= h.hd_compare_bound && EARC_HD_EVENT_CODEC < entries);
        return;
    }
    codec = earc_hd_codec_reg(&h); output = h.hd_output_reg; index = earc_hd_index_reg(&h); name = earc_hd_name_reg(&h);
    bound = h.hd_compare_bound;
    printf("  %s hd sites: shape %d compare=%#llx (cmp w%u,#%llu) supported=%#llx output=x%u name-load=%#llx (x%u <- index x%u) entries=%u\n",
           k->dir, h.hd_shape, h.hd_compare, codec, bound, h.hd_supported_branch, output, h.hd_name_load, name, index, entries);
    /* The supported branch is `str wCODEC,[xOUTPUT,#off]` inside the function,
     * the compare is `cmp wCODEC,#bound`, the name load `ldr xNAME,[x,xINDEX,lsl #3]`. */
    hd_case(k->dir, "hd sites derive, the session accepts the image, and the probed words name the registers",
            h.hd_compare_word == (0x7100001fU | ((ed_u32)bound << 10) | (codec << 5)) &&
            (h.hd_supported_word & 0xffc003ffU) == (0xb9000000U | (output << 5) | codec) &&
            (h.hd_name_word & 0xffe0fc00U) == 0xf8607800U &&
            h.hd_supported_branch > h.cb_output_status &&
            h.hd_supported_branch - h.cb_output_status < 4ULL * k->hdmi[HF__cb_output_status].count);
    hd_case(k->dir, "hd accept acts: codec 7 on the eARC output while ADEC 1 is started on codec 51",
            earc_hd_accept(EARC_HD_EVENT_CODEC, EARC_OUTPUT_EARC, EARC_HD_ADEC_CODEC, 1) && EARC_HD_EVENT_CODEC > bound);
    hd_case(k->dir, "hd accept acts for DTS core too: codec 5 on the eARC output while ADEC 1 is started on codec 48",
            earc_hd_accept(5, 3, 48, 1));
    hd_case(k->dir, "hd accept does not act: the bound, codec 8, mismatched codec pairs, the optical output, ADEC stopped",
            !earc_hd_accept((ed_u32)bound, 3, 51, 1) && !earc_hd_accept(5, 3, 51, 1) && !earc_hd_accept(8, 3, 51, 1) &&
            !earc_hd_accept(7, 2, 51, 1) && !earc_hd_accept(7, 3, 48, 1) && !earc_hd_accept(5, 2, 48, 1) &&
            !earc_hd_accept(7, 3, 51, 0) && !earc_hd_accept(5, 3, 48, 0));
    /* The registers every capture's disassembly shows: cmp w6; str w6,[x19];
     * ldr x7,[x2,x4,lsl #3]. */
    hd_case(k->dir, "hd probe registers: codec w6, output x19, name index x4, name destination x7",
            codec == 6 && output == 19 && index == 4 && name == 7);
    hd_case(k->dir, "hd name acts: index 7, the first index past the table and 2^32+1 are substituted",
            earc_hd_name_substitute(7, entries) && earc_hd_name_substitute(entries, entries) &&
            earc_hd_name_substitute(0x100000001ULL, entries));
    for (i = 0; i < entries; i++) keep = keep && !earc_hd_name_substitute(i, entries);
    hd_case(k->dir, "hd name does not act: every index inside the table keeps LG's load", keep && entries > bound);
    /* The substitute, table[entries-1], as the module reads it: hdmi_entries
     * counted it, so it points at a printable string in the image's data. */
    last_pointer = hdmi_u64(k, h.name_table + 8ULL * (entries - 1));
    last = (const char *)k->image + (last_pointer - k->base);
    printf("  %s hd substitute name: table[%u] = %#llx \"%s\"\n", k->dir, entries - 1, last_pointer, last);
    hd_case(k->dir, "hd substitute name is the table's last entry, the in-image string \"???\"",
            last_pointer >= k->etext && last_pointer - k->etext < 0x4000000ULL && !strcmp(last, "???"));
}

static void good_hd_layout(struct earc_hdmi_layout *h) {
    good_hdmi_layout(h);
    h->hd_shape = 1;
    h->hd_compare_word = 0x710010dfU;   /* cmp w6, #4 */
    h->hd_compare_bound = 4;
    h->hd_supported_word = 0xb9004666U; /* str w6, [x19, #68] */
    h->hd_name_word = 0xf8647847U;      /* ldr x7, [x2, x4, lsl #3] */
    h->hd_output_reg = 19;
}
static void hd_refusal_case(const char *what, const struct earc_hdmi_layout *h, unsigned int entries, const char *expect) {
    const char *r = earc_hd_refusal(h, entries);
    if (expect ? r && strstr(r, expect) : !r) printf("PASS hd refusal (%s): %s\n", what, r ? r : "accepted");
    else { printf("FAIL hd refusal (%s): got %s\n", what, r ? r : "accepted"); failures++; }
}
static void hd_refusal_tests(void) {
    struct earc_hdmi_layout h;
    ed_u64 flags = earc_hd_accept_pstate(0x800003c5ULL);
    good_hd_layout(&h);
    hd_refusal_case("all derived, 6 entries", &h, 6, 0);
    refusal_case("all derived", &h, 0);
    refusal_case("all derived", &h, 0);
    good_hd_layout(&h); h.cb_reason = "holds a breakpoint (another tool is probing it)";
    hd_refusal_case("a breakpoint in _cb_output_status", &h, 6, "holds a breakpoint");
    good_hd_layout(&h); h.cb_reason = "no event-codec acceptance compare";
    hd_refusal_case("compare site not derived", &h, 6, "no event-codec acceptance compare");
    good_hd_layout(&h); h.adec_reason = "adec not derived";
    hd_refusal_case("adec not derived", &h, 6, "adec not derived");
    good_hd_layout(&h); h.hd_compare_bound = 7;
    /* Native kernels (C3/G3, G4: cmp #9, 11 names): mode 7 needs no probe. */
    good_hd_layout(&h); h.hd_compare_bound = 9; h.hd_compare_word = 0x710024dfU;  /* cmp w6, #9 */
    hd_refusal_case("native bound 9, 11 names", &h, 11, 0);
    hd_refusal_case("native bound 9, only 9 names", &h, 9, "smaller than the accepted codecs");
    good_hd_layout(&h); h.hd_compare_bound = 9; h.hd_output_reg = 32; h.hd_name_word |= 31;
    hd_refusal_case("native: probe-site registers are not needed", &h, 11, 0);
    good_hd_layout(&h); h.hd_compare_bound = 9; h.cb_reason = "holds a breakpoint (another tool is probing it)";
    hd_refusal_case("native, but a breakpoint in _cb_output_status", &h, 11, "holds a breakpoint");
    good_hd_layout(&h);
    hd_case("hd native", "only a bound of 7 or more with the codec named inside the table",
            !earc_hd_native(&h, 6) && (h.hd_compare_bound = 9, earc_hd_native(&h, 11)) && !earc_hd_native(&h, 9) &&
            (h.hd_compare_bound = 7, earc_hd_native(&h, 8)));
    good_hd_layout(&h); h.hd_output_reg = 32;
    hd_refusal_case("output register not derived", &h, 6, "output register");
    good_hd_layout(&h); h.hd_name_word |= 31;
    hd_refusal_case("name load into xzr", &h, 6, "SP or ZR");
    good_hd_layout(&h); h.hd_compare_word = 0x90000006U;  /* adrp x6, . */
    hd_refusal_case("compare site holds a PC-relative instruction", &h, 6, "not kprobe-placeable");
    good_hd_layout(&h); h.hd_name_word = 0x58000007U;     /* ldr x7, <literal> */
    hd_refusal_case("name-load site holds a PC-relative instruction", &h, 6, "not kprobe-placeable");
    good_hd_layout(&h);
    hd_refusal_case("name table has only 4 entries", &h, 4, "smaller than the accepted codecs");
    hd_refusal_case("name table not readable", &h, 0, "smaller than the accepted codecs");
    good_hd_layout(&h); h.gate_state = GATE_SWITCHED;
    refusal_case("stock gate already returns 1 (C3/G3, G4, 2026)", &h, 0);
    h.patch_text = 0; h.cpus_read_lock = 0;
    refusal_case("gate already open: nothing to patch, so patch_text/cpus lock not needed", &h, 0);
    good_hd_layout(&h); h.patch_text = 0;
    refusal_case("stock gate and patch_text missing", &h, 1);
    good_hd_layout(&h); h.gate_state = GATE_BREAKPOINT;
    refusal_case("gate holds a breakpoint", &h, 1);
    good_hd_layout(&h); h.gate_state = GATE_UNKNOWN;
    refusal_case("gate in an unknown state", &h, 1);
    good_hd_layout(&h); h.break_reason = "break not derived";
    refusal_case("break not derived", &h, 1);
    refusal_case("break not derived", &h, 1);
    good_hd_layout(&h); h.set_output_reason = "set_output not derived";
    refusal_case("set_output not derived", &h, 1);
    /* The flags written with the redirect: N and V clear, Z and C set (b.ls
     * taken, b.ne not taken), everything else kept. */
    hd_case("hd accept flags", "Z and C set, N and V clear, the other PSTATE bits kept", flags == 0x600003c5ULL);
    hd_case("mode 7 policy", "matches ADEC codec 48 (DTS core) and 51 (DTS-HD)",
            earc_session_policy_match(1, 0x20, 48, 1, 1) && earc_session_policy_match(1, 0x20, 51, 1, 1));
    hd_case("mode 7 policy", "matches no other codec (0, 21, 47, 49, 50, 52)",
            !earc_session_policy_match(1, 0x20, 0, 1, 1) && !earc_session_policy_match(1, 0x20, 21, 1, 1) &&
            !earc_session_policy_match(1, 0x20, 47, 1, 1) && !earc_session_policy_match(1, 0x20, 49, 1, 1) &&
            !earc_session_policy_match(1, 0x20, 50, 1, 1) && !earc_session_policy_match(1, 0x20, 52, 1, 1));
    hd_case("mode 7 policy", "matches decoder 0 as well as 1, for both codecs",
            earc_session_policy_match(0, 0x20, 48, 1, 1) && earc_session_policy_match(0, 0x20, 51, 1, 1));
    hd_case("mode 7 policy", "still needs the eARC mask, open and started, for both codecs and both decoders",
            !earc_session_policy_match(1, 0x14, 51, 1, 1) && !earc_session_policy_match(0, 0x14, 51, 1, 1) &&
            !earc_session_policy_match(1, 0x20, 51, 0, 1) && !earc_session_policy_match(0, 0x20, 51, 0, 1) &&
            !earc_session_policy_match(1, 0x20, 51, 1, 0) && !earc_session_policy_match(0, 0x20, 51, 1, 0) &&
            !earc_session_policy_match(1, 0x14, 48, 1, 1) && !earc_session_policy_match(0, 0x14, 48, 1, 1) &&
            !earc_session_policy_match(1, 0x20, 48, 0, 1) && !earc_session_policy_match(0, 0x20, 48, 0, 1) &&
            !earc_session_policy_match(1, 0x20, 48, 1, 0) && !earc_session_policy_match(0, 0x20, 48, 1, 0));
    hd_case("mode 7 policy", "matches no index without an ADEC slot (2, 4, 12, 0xffffffff, 2^32+1)",
            !earc_session_policy_match(2, 0x20, 51, 1, 1) && !earc_session_policy_match(4, 0x20, 51, 1, 1) &&
            !earc_session_policy_match(12, 0x20, 48, 1, 1) && !earc_session_policy_match(0xffffffffULL, 0x20, 51, 1, 1) &&
            !earc_session_policy_match(0x100000001ULL, 0x20, 48, 1, 1));
    hd_case("app lease policy", "forces the leased renderer on eARC only, and nothing without a lease",
            earc_app_policy_match(1, 0x20, 2) && earc_app_policy_match(0, 0x20, 1) &&
            !earc_app_policy_match(1, 0x20, 0) && !earc_app_policy_match(0, 0x20, 2) &&
            !earc_app_policy_match(1, 0x14, 2) && !earc_app_policy_match(1, 0x20, 1));
    hd_case("hd accept", "each event codec needs its own ADEC codec: 7 with 51, 5 with 48",
            !earc_hd_accept(7, 3, 48, 1) && earc_hd_accept(7, 3, 51, 1) && earc_hd_accept(5, 3, 48, 1) && !earc_hd_accept(5, 3, 51, 1));
}

/* earc_adec_slot gives slot 0 or 1 and nothing for any other index, so the
 * _set_output pre-handler never reads past the two derived slots. */
static void adec_slot_tests(void) {
    static const ed_u64 outside[] = {2, 3, 4, 12, 0xffffffffULL, 0x100000001ULL, ~0ULL};
    struct earc_hdmi_layout h;
    struct earc_adec_slot slot;
    unsigned int i;
    int ok = 1;
    good_hdmi_layout(&h);
    h.adec0_codec = 0x1014; h.adec0_open = 0x1008; h.adec0_started = 0x100a;
    h.adec1_codec = 0x1074; h.adec1_open = 0x1068; h.adec1_started = 0x106a;
    ok = earc_adec_slot(&h, 0, &slot) && slot.codec == 0x1014 && slot.open == 0x1008 && slot.started == 0x100a &&
         earc_adec_slot(&h, 1, &slot) && slot.codec == 0x1074 && slot.open == 0x1068 && slot.started == 0x106a;
    for (i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        slot.codec = slot.open = slot.started = 0x5a5a;
        ok = ok && !earc_adec_slot(&h, outside[i], &slot) && slot.codec == 0x5a5a && slot.open == 0x5a5a && slot.started == 0x5a5a;
    }
    hd_case("adec slot", "index 0 and 1 give their own slot; 2, 3, 4, 12, 0xffffffff, 2^32+1 and 2^64-1 give none and set nothing", ok);
}

/* The DTS-HD acceptance reads the matched decoder's slot, or either slot
 * before any match. */
static void hd_decoder_tests(void) {
    const ed_u32 on1[2] = {0, 51}, on0[2] = {51, 0}, both[2] = {51, 51}, core[2] = {48, 48}, none[2] = {0, 0};
    const ed_u8 up[2] = {1, 1}, down[2] = {0, 0};
    hd_case("hd decoder", "matched 0: accepts on ADEC 0 started on 51, not on a stale ADEC 1",
            earc_hd_accept_decoder(7, 3, 0, on0, up) == 0 && earc_hd_accept_decoder(7, 3, 0, on1, up) == -1);
    hd_case("hd decoder", "matched 1: accepts on ADEC 1 started on 51, not on a stale ADEC 0",
            earc_hd_accept_decoder(7, 3, 1, on1, up) == 1 && earc_hd_accept_decoder(7, 3, 1, on0, up) == -1);
    hd_case("hd decoder", "no match yet: either slot started on 51 accepts, neither refuses",
            earc_hd_accept_decoder(7, 3, EARC_NO_DECODER, on0, up) == 0 && earc_hd_accept_decoder(7, 3, EARC_NO_DECODER, on1, up) == 1 &&
            earc_hd_accept_decoder(7, 3, EARC_NO_DECODER, both, up) == 0 && earc_hd_accept_decoder(7, 3, EARC_NO_DECODER, none, up) == -1);
    hd_case("hd decoder", "any matched value above 1 is no match and reads only slots 0 and 1",
            earc_hd_accept_decoder(7, 3, 4, on1, up) == 1 && earc_hd_accept_decoder(7, 3, 0xffffffffU, on0, up) == 0 &&
            earc_hd_accept_decoder(7, 3, 12, none, up) == -1);
    hd_case("hd decoder", "still refuses a stopped slot, a mismatched codec pair, the optical output",
            earc_hd_accept_decoder(7, 3, 1, on1, down) == -1 && earc_hd_accept_decoder(7, 3, 0, on0, down) == -1 &&
            earc_hd_accept_decoder(7, 3, EARC_NO_DECODER, core, up) == -1 && earc_hd_accept_decoder(5, 3, 1, on1, up) == -1 &&
            earc_hd_accept_decoder(7, 2, 0, on0, up) == -1);
    hd_case("hd decoder", "DTS core: codec 5 accepts on the matched slot started on 48",
            earc_hd_accept_decoder(5, 3, 1, core, up) == 1 && earc_hd_accept_decoder(5, 3, 0, core, up) == 0);
}

#define NOP 0xd503201fU

/* ---- HDMI refusal tests ---- */
static struct earc_hdmi_layout hdmi_last;  /* what the last hdmi_refuse decoded */

/* Copies the HDMI functions, lets mutate change one of them, decodes, and
 * requires reason() to contain expect, or, if expect is null, to be null. */
static void hdmi_refuse(const struct kernel *k, enum earc_hdmi_function which, const char *what, const char *expect,
                        int (*mutate)(ed_u32 *w, ed_u32 n, const struct earc_code *f, const struct earc_hdmi_layout *good),
                        const char *(*reason)(const struct earc_hdmi_layout *), ed_u64 nonpcm, const struct earc_hdmi_layout *good) {
    struct earc_code code[HF_COUNT];
    ed_u32 *copy;
    const char *got;
    memcpy(code, k->hdmi, sizeof(code));
    copy = malloc(4 * code[which].count);
    memcpy(copy, code[which].words, 4 * code[which].count);
    if (!mutate(copy, code[which].count, &code[which], good)) {
        printf("FAIL hdmi refusal (%s): the instruction to mutate was not found\n", what);
        failures++;
        free(copy);
        return;
    }
    code[which].words = copy;
    earc_hdmi_decode(code, nonpcm, k->stext & ~0xfffffULL, &hdmi_last);
    got = reason(&hdmi_last);
    if (expect ? got && strstr(got, expect) : !got) printf("PASS hdmi refusal (%s): %s\n", what, got ? got : "still derived");
    else { printf("FAIL hdmi refusal (%s): got %s\n", what, got ? got : "acceptance"); failures++; }
    free(copy);
}

static const char *gate_reason(const struct earc_hdmi_layout *h) {
    return h->gate_state == GATE_UNKNOWN ? "gate state unknown" : h->gate_state == GATE_BREAKPOINT ? "gate holds a breakpoint" : 0;
}
static const char *setout_reason(const struct earc_hdmi_layout *h) { return h->set_output_reason; }
static const char *adec_reason(const struct earc_hdmi_layout *h) { return h->adec_reason; }
static const char *cb_reason(const struct earc_hdmi_layout *h) { return h->cb_reason; }
static const char *break_reason(const struct earc_hdmi_layout *h) { return h->break_reason; }
static const char *hd_reason(const struct earc_hdmi_layout *h) { return earc_hd_refusal(h, 6); }
static const char *input_reason(const struct earc_hdmi_layout *h) { return h->input_reason; }
static const char *property_reason(const struct earc_hdmi_layout *h) { return h->property_reason; }
static const char *mute_reason(const struct earc_hdmi_layout *h) { return h->mute_reason; }

#define HDMI_MUTATION(name) \
    static int name(ed_u32 *w, ed_u32 n, const struct earc_code *f, const struct earc_hdmi_layout *g)
#define HDMI_UNUSED (void)w; (void)n; (void)f; (void)g

/* mov w0,#0 -> mov w0,#2: neither the stock gate nor the switched one. */
HDMI_MUTATION(gate_third_value) { HDMI_UNUSED; w[0] = 0x52800040U; return 1; }
/* The first instruction -> brk #4, what a kprobe plants. */
HDMI_MUTATION(entry_kprobe) { HDMI_UNUSED; w[0] = HF_KPROBE_BRK; return 1; }
HDMI_MUTATION(input_compare_changed) {
    HDMI_UNUSED;
    if (g->input_compare < f->address || g->input_compare >= f->address + 4ULL * n) return 0;
    w[(g->input_compare - f->address) / 4] = NOP;
    return 1;
}
HDMI_MUTATION(input_scale_changed) {
    ed_u64 compare;
    HDMI_UNUSED;
    compare = (g->input_compare - f->address) / 4;
    if (compare < 3 || compare >= n) return 0;
    w[compare - 3] ^= 1u << 10;
    return 1;
}
HDMI_MUTATION(input_load_destination_changed) {
    ed_u64 compare;
    HDMI_UNUSED;
    compare = (g->input_compare - f->address) / 4;
    if (compare < 2 || compare >= n) return 0;
    w[compare - 2] = (w[compare - 2] & ~31u) | 1u;
    return 1;
}
HDMI_MUTATION(input_table_offset_changed) {
    ed_u64 compare;
    HDMI_UNUSED;
    compare = (g->input_compare - f->address) / 4;
    if (compare < 2 || compare >= n) return 0;
    w[compare - 2] ^= 1u << 10;
    return 1;
}
/* The instruction before the argument load -> mul x9, x9, x9: one input of the
 * table address now comes from an untracked computation. */
HDMI_MUTATION(input_address_unknown) {
    ed_u64 compare;
    HDMI_UNUSED;
    compare = (g->input_compare - f->address) / 4;
    if (compare < 3 || compare >= n) return 0;
    w[compare - 3] = 0x9b097d20u | (w[compare - 3] & 31);
    return 1;
}
HDMI_MUTATION(input_frame_changed) {
    ed_u32 index;
    HDMI_UNUSED;
    for (index = 0; index < n && index < 16; index++)
        if ((w[index] & 0xffc003ffu) == 0xd10003ffu) { w[index] = NOP; return 1; }
    return 0;
}
/* ldr wT,[x1,#mask] -> the same load from x9, which is not an argument. */
HDMI_MUTATION(mask_base_not_argument) {
    ed_u32 i;
    (void)f;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc003e0U) == 0xb9400020U && ((w[i] >> 10) & 0xfff) * 4 == g->mask_offset) { w[i] = (w[i] & ~0x3e0U) | (9U << 5); return 1; }
    return 0;
}
/* The first cmp wA,wB after the mask load -> cmp wA,#1: wA is another word of
 * argument 1 (descriptor+52), now also compared with an immediate. */
HDMI_MUTATION(second_mask_candidate) {
    ed_u32 i, seen = 0;
    (void)f;
    for (i = 0; i < n; i++) {
        if ((w[i] & 0xffc003e0U) == 0xb9400020U && ((w[i] >> 10) & 0xfff) * 4 == g->mask_offset) seen = 1;
        else if (seen && (w[i] & 0xff20fc1fU) == 0x6b00001fU) { w[i] = 0x7100041fU | (w[i] & 0x3e0U); return 1; }
    }
    return 0;
}
/* add xS,xB,xA,lsl #5 -> lsl #6: not the slot scaling the decoder knows. */
HDMI_MUTATION(adec_scaling) {
    ed_u32 i;
    HDMI_UNUSED;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffe0fc00U) == 0x8b001400U) { w[i] = (w[i] & ~(0x3fU << 10)) | (6U << 10); return 1; }
    return 0;
}
/* cmp wI,#1 (before b.hi) -> nop. */
HDMI_MUTATION(adec_no_bound) {
    ed_u32 i;
    HDMI_UNUSED;
    for (i = 0; i + 1 < n; i++)
        if ((w[i] & 0xfffffc1fU) == 0x7100041fU && (w[i + 1] & 0xff00001fU) == 0x54000008U) { w[i] = NOP; return 1; }
    return 0;
}
/* The b.hi after cmp wI,#1 -> cond (b.gt as on ptl4tv.2: equivalent; b.ge would reject index 1). */
static int adec_bound_cond(ed_u32 *w, ed_u32 n, ed_u32 cond) {
    ed_u32 i;
    for (i = 0; i + 1 < n; i++)
        if ((w[i] & 0xfffffc1fU) == 0x7100041fU && (w[i + 1] & 0xff00001fU) == 0x54000008U) { w[i + 1] = (w[i + 1] & ~15U) | cond; return 1; }
    return 0;
}
HDMI_MUTATION(adec_bound_signed) { HDMI_UNUSED; return adec_bound_cond(w, n, 0xc); }
HDMI_MUTATION(adec_bound_ge) { HDMI_UNUSED; return adec_bound_cond(w, n, 0xa); }
/* The codec store: the first str wT,[xS,#off] after `add xS,xB,xA,lsl #5`. */
static ed_u32 adec_store(const ed_u32 *w, ed_u32 n) {
    ed_u32 i, j;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffe0fc00U) == 0x8b001400U) {
            for (j = i + 1; j < n; j++)
                if ((w[j] & 0xffc00000U) == 0xb9000000U && ((w[j] >> 5) & 31) == (w[i] & 31)) return j;
            return n;
        }
    return n;
}
HDMI_MUTATION(adec_no_store) { ed_u32 s = adec_store(w, n); (void)f; (void)g; if (s >= n) return 0; w[s] = NOP; return 1; }
/* The instruction after the codec store -> the same store at slot+24. */
HDMI_MUTATION(adec_two_stores) {
    ed_u32 s = adec_store(w, n);
    (void)f; (void)g;
    if (s + 1 >= n) return 0;
    w[s + 1] = (w[s] & ~(0xfffU << 10)) | (6U << 10);
    return 1;
}
/* The acceptance branch (b.ls on 5.4, b.ne on 6.12) -> b.eq. */
HDMI_MUTATION(cb_branch_condition) {
    ed_u32 c = (ed_u32)((g->hd_compare - f->address) / 4);
    if (c + 1 >= n) return 0;
    w[c + 1] &= ~15U;
    return 1;
}
/* A second acceptance. 5.4: `cmp codec,#bound+1; b.ls supported` written on the
 * fall-through of the first. 6.12: the b.eq after the chain's first compare of
 * the codec, which goes to the supported store, -> b.ls. */
HDMI_MUTATION(cb_second_compare) {
    ed_u32 c = (ed_u32)((g->hd_compare - f->address) / 4), i;
    if (g->hd_shape == 1) {
        if (c + 3 >= n) return 0;
        w[c + 2] = (w[c] & ~(0xfffU << 10)) | ((ed_u32)(g->hd_compare_bound + 1) << 10);
        w[c + 3] = 0x54000009U | ((ed_u32)(((g->hd_supported_branch - (f->address + 4ULL * (c + 3))) >> 2) & 0x7ffff) << 5);
        return 1;
    }
    for (i = 0; i < c; i++)
        if ((w[i] & 0xffc003ffU) == (w[c] & 0xffc003ffU) && (w[i + 1] & 0xff00001fU) == 0x54000000U) { w[i + 1] |= 9; return 1; }
    return 0;
}
HDMI_MUTATION(cb_no_name_load) {
    ed_u32 i = (ed_u32)((g->hd_name_load - f->address) / 4);
    if (i >= n) return 0;
    w[i] = NOP;
    return 1;
}
/* brk #4 at the compare and at the name load, the two sites the tester's HD
 * module probes. */
HDMI_MUTATION(cb_two_probes) {
    ed_u32 c = (ed_u32)((g->hd_compare - f->address) / 4), l = (ed_u32)((g->hd_name_load - f->address) / 4);
    if (c >= n || l >= n) return 0;
    w[c] = w[l] = HF_KPROBE_BRK;
    return 1;
}
/* mov x19,x2 (the output kept across the function) -> mov x19,x1: the register
 * the supported store goes through no longer holds argument 2. */
HDMI_MUTATION(cb_output_not_argument) {
    ed_u32 i;
    (void)f; (void)g;
    for (i = 0; i < n; i++)
        if (w[i] == 0xaa0203f3U) { w[i] = 0xaa0103f3U; return 1; }
    return 0;
}
/* The first RET -> brk #0x800, what the compiler emits for BUG(). */
HDMI_MUTATION(cb_bug_brk) {
    ed_u32 i;
    (void)f; (void)g;
    for (i = 0; i < n; i++)
        if (w[i] == 0xd65f03c0U) { w[i] = 0xd4210000U; return 1; }
    return 0;
}
/* ldr x2,[x1,#56]! -> nop. */
HDMI_MUTATION(break_no_head_load) {
    ed_u32 i;
    (void)f; (void)g;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffe00c00U) == 0xf8400c00U) { w[i] = NOP; return 1; }
    return 0;
}

static void hdmi_refusal_tests(const struct kernel *k, ed_u64 nonpcm, const struct earc_hdmi_layout *good) {
    printf("-- hdmi refusal tests on %s\n", k->dir);
    hdmi_refuse(k, HF_aud_hal_dts_supported, "gate changed to a third value", "gate state unknown", gate_third_value, gate_reason, nonpcm, good);
    hdmi_refuse(k, HF_aud_hal_dts_supported, "kprobe breakpoint on the gate", "gate holds a breakpoint", entry_kprobe, gate_reason, nonpcm, good);
    hdmi_refuse(k, HF__set_output, "kprobe breakpoint at the _set_output entry", "entry holds a breakpoint", entry_kprobe, setout_reason, nonpcm, good);
    hdmi_refuse(k, HF__set_output, "mask load base is not argument 1", "no 32-bit load from argument 1", mask_base_not_argument, setout_reason, nonpcm, good);
    hdmi_refuse(k, HF__set_output, "a second argument-1 word compared with an immediate", "more than one argument-1 word", second_mask_candidate, setout_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_codec, "ADEC slot scaling lsl #5 -> #6", "not found in alsa_decoder_codec", adec_scaling, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_start, "ADEC slot scaling changed in alsa_decoder_start only", "not found in alsa_decoder_start", adec_scaling, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_codec, "ADEC index bound check removed", "bound check", adec_no_bound, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_codec, "ADEC index bound signed (b.gt) is not a refusal", 0, adec_bound_signed, adec_reason, nonpcm, good);
    check("hdmi refusal", "ADEC 1 codec unchanged by a signed bound", hdmi_last.adec1_codec, good->adec1_codec);
    hdmi_refuse(k, HF_alsa_decoder_codec, "ADEC index bound b.ge (rejects index 1)", "bound check", adec_bound_ge, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_codec, "codec store removed", "codec store", adec_no_store, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF_alsa_decoder_codec, "a second 32-bit store through the slot", "more than one 32-bit store", adec_two_stores, adec_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "acceptance branch condition changed", "no event-codec acceptance compare", cb_branch_condition, cb_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "a second acceptance compare", "more than one event-codec acceptance", cb_second_compare, cb_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "name load removed", "codec-name table load", cb_no_name_load, cb_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "kprobe breakpoints at the compare and the name load", "holds a breakpoint", cb_two_probes, cb_reason, nonpcm, good);
    check("hdmi refusal", "breakpoints counted", hdmi_last.hd_brk_count, 2);
    check("hdmi refusal", "first breakpoint offset", hdmi_last.hd_brk_offset,
          (good->hd_compare < good->hd_name_load ? good->hd_compare : good->hd_name_load) - good->cb_output_status);
    hdmi_refuse(k, HF__cb_output_status, "brk #0x800 (BUG) is not a probe", 0, cb_bug_brk, cb_reason, nonpcm, good);
    check("hdmi refusal", "compare unchanged by a BUG brk", hdmi_last.hd_compare, good->hd_compare);
    hdmi_refuse(k, HF_register_kernel_break_hook, "list-head load removed", "list-head load", break_no_head_load, break_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "hd modes: kprobe breakpoints at the compare and the name load", "holds a breakpoint", cb_two_probes, hd_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "hd modes: name load removed", "codec-name table load", cb_no_name_load, hd_reason, nonpcm, good);
    hdmi_refuse(k, HF__cb_output_status, "hd modes: the output register is not argument 2", "output register", cb_output_not_argument, hd_reason, nonpcm, good);
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

/* output_set_earc_on, 5.4: ldr w1, [x19, #76] (the rate) -> cset w1, ne. The
 * clock call's argument 1 is then unknown and nothing loads output+76; the
 * check must not pass because both sides are unknown. */
static int unknown_rate_argument(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    (void)l;
    for (i = 0; i < n; i++)
        if (w[i] == 0xb9404e61U) { w[i] = 0x1a9f07e1U; return 1; }
    return 0;
}
/* output_set_earc_on, 6.12: ldp w2, w1, [x19, #72] -> ldr w1, [x19, #76] (the
 * rate alone) and cset w3, ne -> cset w2, ne: argument 2 is unknown and
 * output+72 is never loaded. */
static int unknown_channel_argument(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i, ldp = 0, cset = 0;
    (void)l;
    for (i = 0; i < n; i++) {
        if (w[i] == 0x29490662U) { w[i] = 0xb9404e61U; ldp = 1; }
        else if (w[i] == 0x1a9f07e3U) { w[i] = 0x1a9f07e2U; cset = 1; }
    }
    return ldp && cset;
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
/* stp wA, wB, [xM, #object rate] -> the same pair at +0x7c: the rate stays
 * inside +0x80, the channel count lands on it. */
static int move_rate_pair_to_edge(ed_u32 *w, ed_u32 n, const struct earc_layout *l) {
    ed_u32 i;
    for (i = 0; i < n; i++)
        if ((w[i] & 0xffc00000U) == 0x29000000U && ((w[i] >> 15) & 0x7f) * 4 == l->clock_object_rate) {
            w[i] = (w[i] & ~(0x7fU << 15)) | ((0x7cU / 4) << 15);
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
    if (!generation)
        refuse(k, EF_output_set_earc_on, "rate argument unknown and output+76 never loaded", "rate at output+76",
               unknown_rate_argument, l);
    if (generation) {
        refuse(k, EF_output_set_earc_on, "channel argument unknown and output+72 never loaded", "channel count at output+72",
               unknown_channel_argument, l);
        follow_stride(k, l);
        refuse(k, EF_aud_clock_update_output, "channel callback load removed", "control+Y", drop_channel_callback_load, l);
        refuse(k, EF_aud_clock_update_output, "rate/channels store removed", "rate (argument 1)", drop_rate_pair, l);
        refuse(k, EF_aud_clock_update_output, "rate/channels store moved onto the flag", "overlap", move_rate_pair, l);
        refuse(k, EF_aud_clock_update_output, "channels store moved to +0x80", "beyond +0x80", move_rate_pair_to_edge, l);
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

static void input_capture_tests(const struct kernel *kernel, const struct earc_hdmi_layout *layout) {
    const struct earc_code *getter = &kernel->hdmi[HF_input_get_hdmi_codec];
    unsigned int index, codec_formats = 0, mode_formats = 0;
    ed_u64 codec_format = 0, mode_format = 0;
    check_reason(kernel->dir, "HDMI input start", layout->input_reason);
    check_reason(kernel->dir, "HDMI input properties", layout->property_reason);
    for (index = 0; index < layout->input_format_count; index++) {
        ed_u64 address = layout->input_formats[index];
        const char *text;
        if (address < kernel->base || address - kernel->base + 16 > kernel->image_size) continue;
        text = (const char *)kernel->image + address - kernel->base;
        if (!strcmp(text, "codec=%d")) { codec_format = address; codec_formats++; }
        if (!strcmp(text, "mode=%d")) { mode_format = address; mode_formats++; }
    }
    check(kernel->dir, "one codec format", codec_formats, 1);
    check(kernel->dir, "one mode format", mode_formats, 1);
    if (!strcmp(kernel->sha256, "46f46c2574d72cf85ff8109f9829d481394a4c173e8fe18389439ec2867f346c")) {
        check(kernel->dir, "input table oracle", layout->input_table, 0xffffffc012742880ULL);
        check(kernel->dir, "start compare oracle", layout->input_compare, 0xffffffc010cfd9e0ULL);
        check(kernel->dir, "decoder lock oracle", layout->decoder_lock, 0xffffffc012411aa8ULL);
        check(kernel->dir, "index register oracle", layout->input_index_reg, 26);
    } else if (!strcmp(kernel->sha256, "4fffc9484aa8c3744688eafd3359ed0edb75988204c94d6b8442dd8098443b6b")) {
        check(kernel->dir, "input table oracle", layout->input_table, 0xffffffc01265a908ULL);
        check(kernel->dir, "start compare oracle", layout->input_compare, 0xffffffc010d02170ULL);
        check(kernel->dir, "decoder lock oracle", layout->decoder_lock, 0xffffffc01232a470ULL);
        check(kernel->dir, "index register oracle", layout->input_index_reg, 26);
    } else {
        check(kernel->dir, "input table oracle", layout->input_table, layout->adec_base - 32);
    }
    check(kernel->dir, "input argument register oracle", layout->input_arg_reg, 19);
    check(kernel->dir, "forced PCM register oracle", layout->input_forced_reg, 23);
    check(kernel->dir, "input frame size oracle", layout->input_frame_size, 160);
    check_reason(kernel->dir, "HDMI format helpers", layout->format_reason);
    check(kernel->dir, "input uid offset oracle", layout->input_uid_offset, 0x44);
    check(kernel->dir, "input codec offset oracle", layout->input_codec_offset, 0x5c);
    check(kernel->dir, "decoder lock calls mutex_lock", layout->decoder_lock_call, kernel_symbol(kernel, "mutex_lock"));
    printf("INFO %s: decoder lock=%llx\n", kernel->dir, layout->decoder_lock);
    if (getter->address == 0xffffffc010ca3be0ULL) {
        check(kernel->dir, "start compare oracle", layout->input_compare, 0xffffffc010cccfe8ULL);
        check(kernel->dir, "codec format oracle", codec_format, 0xffffffc0118e1b18ULL);
        check(kernel->dir, "mode format oracle", mode_format, 0xffffffc0118e8d18ULL);
        check(kernel->dir, "type offset oracle", layout->input_type_offset, 0x7c);
        check(kernel->dir, "index register oracle", layout->input_index_reg, 27);
    } else if (getter->address == 0xffffffc010ca3ca0ULL) {
        check(kernel->dir, "start compare oracle", layout->input_compare, 0xffffffc010cce534ULL);
        check(kernel->dir, "codec format oracle", codec_format, 0xffffffc011841358ULL);
        check(kernel->dir, "mode format oracle", mode_format, 0xffffffc011848940ULL);
        check(kernel->dir, "type offset oracle", layout->input_type_offset, 0x7c);
        check(kernel->dir, "index register oracle", layout->input_index_reg, 27);
    } else if (getter->address == 0xffffffc080f2894cULL) {
        /* C5 43.21.77 (also W25G 43.21.96) shares the code of 43.21.73; its rodata sits 0xa8 higher. */
        ed_u64 rodata = kernel->sha256 && !strncmp(kernel->sha256, "53c3cb3d", 8) ? 0xa8 : 0;
        check(kernel->dir, "start compare oracle", layout->input_compare, 0xffffffc080f55518ULL);
        check(kernel->dir, "codec format oracle", codec_format, 0xffffffc081cc0c78ULL + rodata);
        check(kernel->dir, "mode format oracle", mode_format, 0xffffffc081cc87d8ULL + rodata);
        check(kernel->dir, "type offset oracle", layout->input_type_offset, 0x78);
        check(kernel->dir, "index register oracle", layout->input_index_reg, 28);
    }
    hdmi_refuse(kernel, HF_alsa_decoder_start, "HDMI start compare changed", "not found",
                input_compare_changed, input_reason, 0, layout);
    hdmi_refuse(kernel, HF_alsa_decoder_start, "HDMI input lookup scale changed", "not found",
                input_scale_changed, input_reason, 0, layout);
    hdmi_refuse(kernel, HF_alsa_decoder_start, "HDMI input load destination changed", "not found",
                input_load_destination_changed, input_reason, 0, layout);
    if (getter->address == 0xffffffc010cd2bb0ULL || getter->address == 0xffffffc010cd7f60ULL)
        hdmi_refuse(kernel, HF_alsa_decoder_start, "HDMI input table offset changed", "offsets disagree",
                    input_table_offset_changed, input_reason, 0, layout);
    hdmi_refuse(kernel, HF_alsa_decoder_start, "HDMI input address not a table lookup", "not found",
                input_address_unknown, input_reason, 0, layout);
    hdmi_refuse(kernel, HF_input_get_hdmi_codec, "HDMI getter breakpoint", "not a word load",
                entry_kprobe, input_reason, 0, layout);
    hdmi_refuse(kernel, HF_input_set_hdmi, "HDMI stack frame changed", "ambiguous",
                input_frame_changed, property_reason, 0, layout);
    hdmi_refuse(kernel, HF_aimc_set_property, "HDMI property breakpoint", "not derived",
                entry_kprobe, property_reason, 0, layout);
    hdmi_refuse(kernel, HF_sndout_mute_input, "input mute entry breakpoint", "not derived",
                entry_kprobe, mute_reason, 0, layout);
}

/* The lease device's write/open/release slots, derived from the kernel's write
 * path, misc_open and __fput, on every kernel: 5.4 measured 0x18/0x70/0x80,
 * 6.12 0x20/0x68/0x78 by hand. */
static void fops_test(const struct kernel *k, const struct earc_hdmi_layout *h) {
    int v612 = k->stext >= 0xffffffc080000000ULL;
    unsigned int want = v612 ? 0x68 : 0x70;
    check_reason(k->dir, "file_operations slots", h->fops_reason);
    check(k->dir, "file_operations write slot", h->fops_write, v612 ? 0x20 : 0x18);
    check(k->dir, "file_operations open slot", h->fops_open, want);
    check(k->dir, "file_operations release slot", h->fops_release, want + 16);
    {
        struct earc_code changed[HF_COUNT];
        struct earc_hdmi_layout m;
        ed_u32 copy[4096];
        unsigned int i;
        memcpy(changed, k->hdmi, sizeof(changed));
        for (i = 0; i < changed[HF___fput].count && i < 4096; i++) copy[i] = changed[HF___fput].words[i];
        /* A second release call in __fput (f_op load, slot load, cbz, blr): ambiguous, refused. */
        for (i = 1; i + 5 < changed[HF___fput].count; i++)
            if ((copy[i] & 0xffc00000u) == 0xf9400000u && ((copy[i] >> 10) & 0xfff) * 8 == h->fops_release) {
                unsigned int at = changed[HF___fput].count - 6, k;
                for (k = 0; k < 6; k++) copy[at + k] = copy[i - 1 + k];
                copy[at + 2] = (copy[at + 2] & 0xff00001fu) | (1u << 5);  /* cbz to the next word */
                break;
            }
        changed[HF___fput].words = copy;
        earc_hdmi_decode(changed, 0, k->stext & ~0xfffffULL, &m);
        check(k->dir, "duplicated release call refuses", m.fops_reason != 0, 1);
    }
}

static void input_policy_tests(void) {
    check("input mute", "active DTS bypass", earc_input_unmute(1, 1, 1, 1, 48, 1, 1), 1);
    check("input mute", "active HD bypass", earc_input_unmute(1, 0, 1, 0, 51, 1, 1), 1);
    check("input mute", "stopped decoder", earc_input_unmute(1, 1, 1, 1, 48, 1, 0), 0);
    check("input mute", "closed decoder", earc_input_unmute(1, 1, 1, 1, 48, 0, 1), 0);
    check("input mute", "PCM decoder", earc_input_unmute(1, 1, 1, 1, 1, 1, 1), 0);
    check("input mute", "other renderer", earc_input_unmute(1, 0, 1, 1, 48, 1, 1), 0);
    check("input mute", "AMIXER input", earc_input_unmute(2, 1, 1, 1, 48, 1, 1), 0);
    check("input mute", "unmute unchanged", earc_input_unmute(1, 1, 0, 1, 48, 1, 1), 0);
    check("input policy", "DTS start", earc_input_start_match(1, 48, 1, 1), 1);
    check("input policy", "HD start", earc_input_start_match(0, 51, 1, 1), 1);
    check("input policy", "closed decoder", earc_input_start_match(1, 48, 0, 1), 0);
    check("input policy", "PCM decoder", earc_input_start_match(1, 1, 1, 1), 0);
    check("input policy", "invalid index", earc_input_start_match(2, 48, 1, 1), 0);
    check("input policy", "native input", earc_input_start_match(1, 48, 1, 17), 0);
    check("input policy", "core codec", earc_input_property(4, 48000, 0, 0), 5);
    check("input policy", "HD codec", earc_input_property(202, 192000, 0, 0), 6);
    check("input policy", "HD mode", earc_input_property(202, 192000, 1, 1), 4);
    check("input policy", "unknown HD mode", earc_input_property(1202, 192000, 1, 1), 7);
    check("input policy", "PCM mode", earc_input_property(202, 192000, 1, 0), 0);
    check("input policy", "native codec", earc_input_property(4, 48000, 0, 5), 5);
    check("input policy", "native HRA", earc_input_property(203, 192000, 1, 5), 5);
    check("input policy", "HRA mode", earc_input_property(203, 48000, 1, 1), 5);
    check("input policy", "unknown HRA mode", earc_input_property(1203, 48000, 1, 1), 8);
    check("input policy", "HRA low rate", earc_input_property(203, 48000, 0, 0), 5);
    check("input policy", "HRA high rate", earc_input_property(203, 192000, 0, 0), 6);
    check("input policy", "AC3", earc_input_property(3, 48000, 0, 0), 0);
}

/* A kernel with no oracle: if passthrough_init's order of reasons refuses it,
 * print the reason and skip the checks that assume an accepted image. */
static int hdmi_session_refusal(const struct kernel *k, struct earc_layout *l) {
    struct earc_hdmi_layout h;
    const char *reason = decode(k->code, l);
    if (!reason) {
        earc_hdmi_decode(k->hdmi, l->nonpcm, k->stext & ~0xfffffULL, &h);
        reason = earc_mode_refusal(&h);
        if (!reason) reason = h.mute_reason;
        if (!reason) reason = h.input_reason;
        if (!reason) reason = h.property_reason;
        if (!reason) reason = h.format_reason;
    }
    if (reason) printf("REFUSE %s: the module refuses this kernel: %s\n", k->dir, reason);
    return reason != 0;
}

int main(int argc, char **argv) {
    struct kernel kernels[16];
    struct earc_layout layouts[16];
    struct earc_hdmi_layout hdmi_layout;
    /* The first decoded oracle kernel of each (generation, family). */
    int first[2][3] = {{-1, -1, -1}, {-1, -1, -1}}, refused[2] = {0, 0}, hdmi_refused[2] = {0, 0};
    int i, n = 0;
    for (i = 1; i + 2 < argc && n < 16; i += 3) {
        const char *sha = argv[i + 1], *dir = argv[i + 2];
        const struct oracle *o = 0;
        const struct hdmi_oracle *ho = 0;
        size_t j;
        int loaded;
        if (strcmp(argv[i], "--kernel")) { printf("FAIL usage: test_decode [--kernel SHA256 DIR]...\n"); return 2; }
        for (j = 0; j < sizeof(oracles) / sizeof(oracles[0]); j++)
            if (!strcmp(oracles[j].sha256, sha)) o = &oracles[j];
        for (j = 0; j < sizeof(hdmi_oracles) / sizeof(hdmi_oracles[0]); j++)
            if (!strcmp(hdmi_oracles[j].sha256, sha)) ho = &hdmi_oracles[j];
        loaded = load_kernel(&kernels[n], dir);
        /* A kernel without the module's symbols (another audio stack) is refused, as the loader refuses it. */
        if (loaded == -2 && !o && !ho) continue;
        if (loaded < 0) { failures++; continue; }
        if (!loaded) continue;
        kernels[n].sha256 = sha;
        if (!o && !ho && hdmi_session_refusal(&kernels[n], &layouts[n])) {
            if (!strcmp(sha, "46f46c2574d72cf85ff8109f9829d481394a4c173e8fe18389439ec2867f346c") ||
                !strcmp(sha, "4fffc9484aa8c3744688eafd3359ed0edb75988204c94d6b8442dd8098443b6b")) {
                printf("FAIL %s: supported C2/C4 image was refused\n", dir);
                failures++;
            }
            n++;
            continue;
        }
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
        /* The HDMI oracle runs whenever its sha matches, with or without an
         * earc oracle for the same image (the CS kernel has an HDMI oracle but
         * no earc one). layouts[n].nonpcm is set by run_oracle or the decode
         * above. */
        hdmi_validity_test(&kernels[n]);
         earc_hdmi_decode(kernels[n].hdmi, layouts[n].nonpcm, kernels[n].stext & ~0xfffffULL, &hdmi_layout);
        input_capture_tests(&kernels[n], &hdmi_layout);
         printf("INFO %s: HDMI start compare=%llx index=x%u\n", dir,
             hdmi_layout.input_compare, hdmi_layout.input_index_reg);
         printf("INFO %s: HDMI property entry=%llx frame=%u type=sp+%u forced=x%u formats=%u\n", dir,
             hdmi_layout.property_entry, hdmi_layout.input_frame_size, hdmi_layout.input_type_offset,
             hdmi_layout.input_forced_reg, hdmi_layout.input_format_count);
        fops_test(&kernels[n], &hdmi_layout);
        check_reason(dir, "input mute layout", hdmi_layout.mute_reason);
        check(dir, "input mute request offset", hdmi_layout.mute_offset, 0x48);
        check_reason(dir, "eARC mix flag", hdmi_layout.mix_reason);
        check(dir, "eARC mix flag offset", hdmi_layout.mix_offset, 0x68);
        hd_probe_test(&kernels[n], layouts[n].nonpcm);
        if (ho && run_hdmi_oracle(ho, &kernels[n], layouts[n].nonpcm, &hdmi_layout) && !hdmi_refused[ho->generation]) {
            hdmi_refusal_tests(&kernels[n], layouts[n].nonpcm, &hdmi_layout);
            hdmi_refused[ho->generation] = 1;
        }
        n++;
    }
    if (argc < 2 || (argc - 1) % 3) { printf("FAIL usage: test_decode [--kernel SHA256 DIR]...\n"); return 2; }
    for (i = 0; i < 2; i++) {
        if (first[i][0] >= 0 && first[i][1] >= 0) difference_test(i ? "6.12" : "5.4", &layouts[first[i][0]], &layouts[first[i][1]]);
        else printf("SKIP %s C5/G5 difference test: needs both oracle kernels\n", i ? "6.12" : "5.4");
        if (!refused[i]) printf("SKIP %s refusal tests: no oracle kernel decoded\n", i ? "6.12" : "5.4");
    }
    hdmi_rule_test();
    hdmi_walk_tests();
    hook_decision_tests();
    gate_tests();
    mode_refusal_tests();
    hd_refusal_tests();
    adec_slot_tests();
    hd_decoder_tests();
    input_policy_tests();
    if (first[0][0] >= 0 && first[1][0] >= 0) generation_test("C5", &layouts[first[0][0]], &layouts[first[1][0]]);
    else printf("SKIP C5 5.4/6.12 difference test: needs both oracle kernels\n");
    if (first[0][1] >= 0 && first[1][1] >= 0) generation_test("G5", &layouts[first[0][1]], &layouts[first[1][1]]);
    else printf("SKIP G5 5.4/6.12 difference test: needs both oracle kernels\n");
    printf(failures ? "RESULT: %d failure(s)\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
