/* SPDX-License-Identifier: GPL-2.0-only */
/* eARC route for rooted LG webOS 25 TVs on the 5.4.268 kernel generation.
 *
 * Port of Glasshouse's native-route.c (github.com/rorygallagher2024/glasshouse,
 * native/dts-forwarding), written and qualified by Glasshouse on the G5. The
 * route logic, producer lease, watchdog, HD mode, restore and hand-back paths
 * are theirs; the C5 port re-derived every address by hand. This version
 * hardcodes none: the loader passes each LG function's address and size from
 * /proc/kallsyms (fn_<name>, sz_<name>), and earc_decode.h derives every data
 * address from LG's own code at load time. Any mismatch refuses the load.
 *
 * mode=0 (default) audits: it prints every address and derived value and the
 * read-only eARC state, then returns -ECANCELED so the kernel unloads it at
 * once. mode=1 routes renderer <renderer> to eARC output 3 under a producer
 * lease, as Glasshouse's apply build does. The struct module layout and
 * vermagic are those of the stock 5.4.268 modules and are checked against
 * them at build and load time. */
#include "earc_decode.h"

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef void *(*block_fn)(const char *, unsigned int);
typedef int (*connection_fn)(void *, unsigned int, void *, unsigned int);
typedef int (*disconnect_fn)(void *, unsigned int);
typedef void (*format_fn)(unsigned int, unsigned int);
extern int printk(const char *format, ...);
extern void *kthread_create_on_node(int (*function)(void *), void *data, int node, const char *name, ...);
extern int wake_up_process(void *task);
extern int kthread_stop(void *task);
extern _Bool kthread_should_stop(void);
extern void msleep(unsigned int milliseconds);
extern void mutex_lock(void *mutex);
extern void mutex_unlock(void *mutex);

/* Parameters set by earc-route-load.sh. */
#define EARC_ADDRESS(name, analysed) static unsigned long fn_##name;
EARC_FUNCTIONS(EARC_ADDRESS)
#undef EARC_ADDRESS
#define EARC_SIZE_1(name) static unsigned int sz_##name;
#define EARC_SIZE_0(name)
#define EARC_SIZE(name, analysed) EARC_SIZE_##analysed(name)
EARC_FUNCTIONS(EARC_SIZE)
#undef EARC_SIZE
static unsigned long fn__stext, fn__etext;
static u32 mode, renderer;

static const struct {
    const char *name;
    unsigned long *address;
    unsigned int *size;
} functions[EF_COUNT] = {
#define EARC_SIZE_POINTER_1(name) &sz_##name
#define EARC_SIZE_POINTER_0(name) 0
#define EARC_ROW(name, analysed) {#name, &fn_##name, EARC_SIZE_POINTER_##analysed(name)},
    EARC_FUNCTIONS(EARC_ROW)
#undef EARC_ROW
};

/* The exported functions as the kernel's module loader resolved them, to
 * check the loader's kallsyms reading. Volatile keeps them real loads. */
static void (*const volatile imported_mutex_lock)(void *) = mutex_lock;
static void (*const volatile imported_mutex_unlock)(void *) = mutex_unlock;

static struct earc_layout layout;

#define BM_GET ((block_fn)fn_bm_get_block_info_by_index)
#define FIND_BLOCK ((void *(*)(void *))fn__bm_find_block_by_block_info)
#define GET_PORT ((u8 *(*)(void *, u32, u8))fn__bm_get_block_port)
#define CONNECT_LOCKED ((connection_fn)fn__bm_connect_block_locked)
#define DISCONNECT_LOCKED ((disconnect_fn)fn__bm_disconnect_block_locked)
#define HAL_FORMAT ((format_fn)fn_audio_hal_earc_set_output_format)
#define HAL_CHANNEL ((format_fn)fn_audio_hal_earc_set_channel)
#define HAL_MUTE ((void (*)(u32))fn_audio_hal_earc_set_mute)
#define HAL_AUDIO_MODE ((void (*)(u32))fn_audio_hal_earc_set_audio_mode)
#define HAL_SET_ON ((void (*)(u32))fn_audio_hal_earc_set_on)
#define UPDATE_CLOCK ((int (*)(u32, u32, u32))fn_aud_clock_update_output)
#define SEND_COMMAND ((int (*)(void *, u32, u32, u32, void *, u32))fn_aimc_send_command)
#define PCM_HEAD ((u32 (*)(void *))fn_renderer_get_uid_pcm_bypass_head)
#define CLOCK_CALLBACK ((void *)fn_aud_hal_set_arc_sample_rate)
#define BM_MUTEX ((void *)layout.bm_mutex)
#define WORD(field) (*(volatile u32 *)layout.field)

/* Connection walk; the offsets are those earc_decode checked in
 * _bm_disconnect_block_locked: port connections +48, peer port +16, owner
 * +32, port ID +40, public block-info +88. The block-manager mutex protects
 * the walk. */
static int output_edge_for(void *output, void **source, u32 *source_port) {
    void *node = FIND_BLOCK(output);
    u8 *port, *head, *entry, *peer, *owner;
    if (!node) return -1;
    port = GET_PORT(node, 0, 1);
    if (!port) return -1;
    head = port + 48;
    entry = *(u8 **)head;
    if (entry == head) return 0;
    if (!entry || *(u8 **)entry != head || *(u8 **)(head + 8) != entry) return -1;
    peer = *(u8 **)(entry + 16);
    if (!peer) return -1;
    owner = *(u8 **)(peer + 32);
    if (!owner) return -1;
    *source = *(void **)(owner + 88);
    *source_port = *(u32 *)(peer + 40);
    return 1;
}

static int valid_output_rate(u32 rate) {
    return rate == 32000 || rate == 44100 || rate == 48000 || rate == 64000 || rate == 88200 ||
        rate == 96000 || rate == 128000 || rate == 176400 || rate == 192000;
}

/* Output clock entry 3 (the static "arc" entry), with the field offsets
 * earc_decode checked in aud_clock_update_output: entry[8] says its mutex at
 * +0x38 is initialised, entry[9] that the output is enabled; the object at
 * +16 holds the rate at +12, the flag at +16 and the control at +128, whose
 * +32 is the callback. */
static int output_clock(u32 *rate, u32 *flag) {
    u8 *entry = (u8 *)layout.clock_entry3, *object, *control;
    int result = -19;
    if (!entry[8]) return result;
    mutex_lock(entry + 0x38);
    object = *(u8 **)(entry + 16);
    if (entry[9] && object) {
        control = *(u8 **)(object + 128);
        if (control && *(void **)(control + 32) == CLOCK_CALLBACK) {
            *rate = *(u32 *)(object + 12);
            *flag = object[16];
            if (valid_output_rate(*rate) && *flag <= 1) result = 0;
        }
    }
    mutex_unlock(entry + 0x38);
    return result;
}

static void *source_block, *old_source_block, *output_block, *watchdog;
static u32 original_nonpcm, original_rate;
static int applied;
static u32 lease = 1;
static u32 restore_pcm;
/* The eARC sample rate for the IEC 61937 carrier: 48000 for the DTS core,
 * 192000 for the eight-channel DTS-HD (HBR) carrier. */
static u32 carrier_rate = 48000;
static u32 hd_ready;
static u32 original_clock_rate, original_clock_flag, original_channels, original_acmod;
static u32 original_audio_mode, original_hal_mute, original_earc_on;
static int hd_saved;

static int output_edge(void **source, u32 *source_port) {
    return output_edge_for(output_block, source, source_port);
}

static int owns_route_locked(void) {
    void *source = (void *)0;
    u32 port = 0;
    return BM_GET("renderer", renderer) == source_block && BM_GET("output", 3) == output_block &&
        output_edge(&source, &port) == 1 && source == source_block && port == 0x100;
}

static int owns_route(void) {
    int owned;
    mutex_lock(BM_MUTEX);
    owned = owns_route_locked();
    mutex_unlock(BM_MUTEX);
    return owned;
}

static int hd_format_matches(void) {
    u32 rate = 0, flag = 0;
    return !output_clock(&rate, &flag) && rate == 192000 && flag == original_clock_flag &&
        WORD(nonpcm) == 1 && WORD(rate) == 192000 &&
        (WORD(control) & 7) == 6 && ((WORD(cs0) >> 24) & 15) == 9 && (WORD(cs0) & 2);
}

/* The clock callback and link reprogramming sleep. Neither block-manager nor
 * clock-table mutex may be held across them. The producer lease owns the UID. */
static int configure_hd(void) {
    u32 uid, format[8] = {192000, 8, 16, 0, 0, 192000, 8, 32};
    int result;
    mutex_lock(BM_MUTEX);
    if (!owns_route_locked() || !*((u8 *)source_block + 77)) {
        mutex_unlock(BM_MUTEX);
        return -16;
    }
    HAL_MUTE(1);
    __atomic_store_n(&hd_ready, 0, __ATOMIC_RELEASE);
    uid = *(u32 *)((u8 *)source_block + 52);
    mutex_unlock(BM_MUTEX);
    result = SEND_COMMAND((void *)0, 0x50020020, 0, uid, format, sizeof(format));
    if (result < 0) return result;
    msleep(100);
    if (!owns_route()) return -16;
    result = UPDATE_CLOCK(3, 192000, original_clock_flag);
    if (result < 0 || !owns_route()) return result < 0 ? result : -16;
    HAL_MUTE(1);
    HAL_CHANNEL(8, 2);
    HAL_FORMAT(1, 192000);
    HAL_CHANNEL(8, 2);
    HAL_SET_ON(original_earc_on);
    printk("\0016earc_route: HD srcbin=%08x, output clock and eARC link configured\n", uid);
    return 0;
}

struct output_state {
    void *output;
    u32 codec, channels, rate, acmod, flag, on;
};

static int same_output_state(const struct output_state *a, const struct output_state *b) {
    return a->output == b->output && a->codec == b->codec && a->channels == b->channels &&
        a->rate == b->rate && a->acmod == b->acmod && a->flag == b->flag && a->on == b->on;
}

static void copy_output_state(void *output, struct output_state *state) {
    volatile u8 *bytes = output;
    state->output = output;
    state->codec = *(volatile u32 *)(bytes + 68);
    state->channels = *(volatile u32 *)(bytes + 72);
    state->rate = *(volatile u32 *)(bytes + 76);
    state->acmod = *(volatile u32 *)(bytes + 80);
    state->flag = *(volatile u32 *)(bytes + 84);
    state->on = bytes[129];
}

static int foreign_output_state(struct output_state *state, int *foreign) {
    struct output_state repeated;
    void *output, *source = (void *)0;
    u32 port = 0;
    int edge, valid = 0;
    *foreign = 0;
    mutex_lock(BM_MUTEX);
    output = BM_GET("output", 3);
    if (output && *(u32 *)((u8 *)output + 44) == 3) {
        edge = output_edge_for(output, &source, &port);
        /* An ambiguous input must stay muted: it could still mix our carrier
         * with another source. A single foreign edge belongs entirely to LG. */
        if (edge == 0 || (edge == 1 && (source != source_block || port != 0x100))) {
            *foreign = 1;
            copy_output_state(output, state);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            copy_output_state(output, &repeated);
            valid = same_output_state(state, &repeated) && state->codec <= 4 &&
                state->channels > 0 && state->channels <= 8 && valid_output_rate(state->rate) &&
                state->acmod <= 7 && state->flag <= 1 && state->on <= 1;
        }
    }
    mutex_unlock(BM_MUTEX);
    return valid;
}

/* LG suppresses an output status equal to its cache. Replaying its current
 * tuple returns global clock/mute state even when the new source is PCM48
 * like the old one; the foreign connection itself is never changed. */
static void hand_back_output(void) {
    struct output_state state, current;
    u32 rate = 0, flag = 0;
    unsigned int attempt;
    int foreign = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        if (!foreign_output_state(&state, &foreign) || output_clock(&rate, &flag)) {
            msleep(50);
            continue;
        }
        UPDATE_CLOCK(3, state.rate, state.flag);
        HAL_MUTE(1);
        HAL_CHANNEL(state.channels, state.acmod);
        HAL_FORMAT(state.codec != 0, state.rate);
        HAL_CHANNEL(state.channels, state.acmod);
        if (!foreign_output_state(&current, &foreign) || !same_output_state(&state, &current)) continue;
        HAL_MUTE(0);
        HAL_SET_ON(state.on);
        if (foreign_output_state(&current, &foreign) && same_output_state(&state, &current)) {
            printk("\0016earc_route: returned output to LG codec=%u channels=%u rate=%u\n",
                   state.codec, state.channels, state.rate);
            return;
        }
    }
    if (foreign && (WORD(cs2) & (1U << 18))) HAL_MUTE(0);
    printk("\0013earc_route: LG output handback did not settle; foreign=%d\n", foreign);
}

static void restore_route(void) {
    void *current_source = (void *)0, *restored_source = (void *)0;
    u32 current_port = 0;
    int edge, restored = 0, restore_format = 0;
    if (!__atomic_exchange_n(&applied, 0, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&hd_ready, 0, __ATOMIC_RELEASE);
    mutex_lock(BM_MUTEX);
    if (BM_GET("output", 3) == output_block) {
        edge = output_edge(&current_source, &current_port);
        /* A removed renderer leaves an empty input. A foreign connection
         * belongs to LG's new source and must survive module cleanup. */
        if (edge == 0 || (edge == 1 && current_source == source_block && current_port == 0x100)) {
            void *current_baseline = BM_GET(restore_pcm ? "mixer" : "encoder", restore_pcm ? 0 : 3);
            int disconnected = edge == 0 || DISCONNECT_LOCKED(output_block, 0) >= 0;
            if (disconnected) {
                restore_format = 1;
                if (hd_saved) HAL_MUTE(1);
                if (current_baseline) restored = CONNECT_LOCKED(current_baseline, 0, output_block, 0) >= 0;
                if (restored) restored_source = current_baseline;
            }
        }
    }
    if (restore_format) {
        if (hd_saved) HAL_CHANNEL(original_channels, original_acmod);
        HAL_FORMAT(original_nonpcm, original_rate);
        if (hd_saved) {
            HAL_CHANNEL(original_channels, original_acmod);
            HAL_AUDIO_MODE(original_audio_mode);
        }
    }
    mutex_unlock(BM_MUTEX);
    if (restore_format && hd_saved) {
        u32 rate = 0, flag = 0;
        int still_owned, result;
        current_source = (void *)0;
        current_port = 0;
        mutex_lock(BM_MUTEX);
        edge = BM_GET("output", 3) == output_block ? output_edge(&current_source, &current_port) : -1;
        still_owned = edge == 0 || (edge == 1 && restored_source && current_source == restored_source && current_port == 0);
        mutex_unlock(BM_MUTEX);
        if (!still_owned) {
            hand_back_output();
            return;
        }
        result = UPDATE_CLOCK(3, original_clock_rate, original_clock_flag);
        HAL_MUTE(original_hal_mute);
        HAL_SET_ON(original_earc_on);
        msleep(100);
        printk("\0016earc_route: HD restore clock=%u flag=%u result=%d readback=%d\n",
               original_clock_rate, original_clock_flag, result,
               !output_clock(&rate, &flag) && rate == original_clock_rate && flag == original_clock_flag);
    } else if (hd_saved) {
        hand_back_output();
    }
    printk(restored ? "\0016earc_route: original output route restored\n" :
           restore_format ? "\0013earc_route: carrier disconnected and format restored; baseline source unavailable\n" :
           "\0016earc_route: output ownership changed; preserving current route\n");
}

static int watch_route(void *unused) {
    unsigned int tick = 0, previous = lease, phase = 0, stable = 0, attempts = 0;
    (void)unused;
    while (tick < 200 && !kthread_should_stop()) {
        msleep(100);
        tick++;
        if (__atomic_load_n(&lease, __ATOMIC_ACQUIRE) != previous) {
            previous = __atomic_load_n(&lease, __ATOMIC_ACQUIRE);
            tick = 0;
        }
        if (!owns_route()) {
            printk("\0016earc_route: output ownership lost\n");
            break;
        }
        if (carrier_rate == 48000) {
            if (WORD(nonpcm) != 1 || WORD(rate) != 48000) break;
            continue;
        }
        if (phase && !hd_format_matches()) {
            u32 rate = 0, flag = 0;
            HAL_MUTE(1);
            __atomic_store_n(&hd_ready, 0, __ATOMIC_RELEASE);
            output_clock(&rate, &flag);
            printk("\0016earc_route: HD format changed mode=%u CS0=%08x clock=%u flag=%u\n",
                   WORD(control) & 7, WORD(cs0), rate, flag);
            phase = stable = 0;
        }
        if (!phase) {
            if (++stable < 2) continue;
            if (++attempts > 8) {
                printk("\0013earc_route: HD format recovery attempts exhausted\n");
                break;
            }
            if (configure_hd() < 0) {
                printk("\0013earc_route: HD setup lost its route or clock\n");
                break;
            }
            phase = 1;
            stable = 0;
        } else if (phase == 1 && ++stable >= 2) {
            if (!owns_route() || !hd_format_matches()) continue;
            HAL_MUTE(original_hal_mute);
            __atomic_store_n(&hd_ready, 1, __ATOMIC_RELEASE);
            phase = 2;
            stable = 0;
            printk("\0016earc_route: HD ready after stable output format\n");
        } else if (phase == 2 && ++stable >= 50) {
            attempts = 0;
            stable = 0;
        }
    }
    restore_route();
    /* Keep the task alive until module removal owns kthread_stop(). */
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* Read-only state. The heap objects read here are block-infos looked up and
 * read under the block-manager mutex, which bm_destroy_block holds while it
 * frees them, and the clock object read by output_clock under its entry
 * mutex. Nothing is written. */
static void audit(void) {
    u8 *entry = (u8 *)layout.clock_entry3, *bytes;
    void *source = (void *)0;
    u32 rate = 0, flag = 0, port = 0;
    unsigned int index;
    int clock = output_clock(&rate, &flag), edge;
    printk("\0016earc_route: HAL mode-select=%u (1 selects mode 7, not 6)\n", WORD(mode_select));
    printk("\0016earc_route: clock3 ready=%u enabled=%u arc-callback=%d result=%d rate=%u flag=%u\n",
           entry[8], entry[9], *(void **)(entry + 32) == CLOCK_CALLBACK, clock, rate, flag);
    mutex_lock(BM_MUTEX);
    for (index = 0; index < 2; index++) {
        bytes = BM_GET("renderer", index);
        if (bytes)
            printk("\0016earc_route: renderer%u head=%08x main=%08x srcbin=%08x bypass-pipeline=%u pcm-bypass=%u\n",
                   index, *(u32 *)(bytes + 44), *(u32 *)(bytes + 48), *(u32 *)(bytes + 52), bytes[76], bytes[77]);
    }
    bytes = BM_GET("output", 3);
    if (bytes) {
        edge = output_edge_for(bytes, &source, &port);
        printk("\0016earc_route: output3 type=%u codec=%u channels=%u rate=%u acmod=%u flag=%u on=%u edge=%d source=%px port=%x\n",
               *(u32 *)(bytes + 44), *(u32 *)(bytes + 68), *(u32 *)(bytes + 72), *(u32 *)(bytes + 76),
               *(u32 *)(bytes + 80), *(u32 *)(bytes + 84), bytes[129], edge, source, port);
    }
    mutex_unlock(BM_MUTEX);
}

/* Parameter checks before any LG text is read. */
static const char *check_parameters(void) {
    unsigned int i;
    if (!fn__stext || fn__stext >= fn__etext || fn__etext - fn__stext > 0x10000000UL)
        return "fn__stext/fn__etext missing or implausible";
    for (i = 0; i < EF_COUNT; i++) {
        unsigned long address = *functions[i].address;
        unsigned int size = functions[i].size ? *functions[i].size : 0;
        if (!address || (address & 3) || address < fn__stext || address >= fn__etext) {
            printk("\0013earc_route: fn_%s=%lx missing or outside kernel text\n", functions[i].name, address);
            return "a function address is missing or outside kernel text";
        }
        if (functions[i].size && (!size || (size & 3) || size > 4 * ED_MAX_INSNS || size > fn__etext - address)) {
            printk("\0013earc_route: sz_%s=%u missing or out of range\n", functions[i].name, size);
            return "a function size is missing or out of range";
        }
    }
    if (fn_mutex_lock != (unsigned long)imported_mutex_lock || fn_mutex_unlock != (unsigned long)imported_mutex_unlock)
        return "fn_mutex_lock/fn_mutex_unlock differ from the kernel's exported symbols";
    if (mode > 1 || renderer > 1 || restore_pcm > 1 || (carrier_rate != 48000 && carrier_rate != 192000))
        return "mode, renderer, restore_pcm or carrier_rate out of range";
    return 0;
}

/* Derived data must be aligned and lie in the kernel's data, which follows its
 * text (21.5 MiB past _etext on the C5, 19 MiB on the G5, for images of about
 * 38 MiB). */
static const char *check_layout(void) {
    const ed_u64 words[] = {layout.nonpcm, layout.cs0, layout.cs2, layout.mode_select, layout.rate,
                            layout.channel_count, layout.acmod, layout.control};
    unsigned int i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if ((words[i] & 3) || words[i] < fn__etext || words[i] - fn__etext >= 0x4000000UL)
            return "a derived HAL word is misaligned or outside the kernel's data";
    if ((layout.bm_mutex & 7) || layout.bm_mutex < fn__etext || layout.bm_mutex - fn__etext >= 0x4000000UL)
        return "the derived block-manager mutex is misaligned or outside the kernel's data";
    if ((layout.clock_entry3 & 7) || layout.clock_entry3 < fn__etext || layout.clock_entry3 - fn__etext >= 0x4000000UL)
        return "the derived clock entry is misaligned or outside the kernel's data";
    return 0;
}

static void print_decoded(void) {
    unsigned int i;
    for (i = 0; i < EF_COUNT; i++)
        printk("\0016earc_route: %s=%lx size=%u\n", functions[i].name, *functions[i].address,
               functions[i].size ? *functions[i].size : 0);
    printk("\0016earc_route: _stext=%lx _etext=%lx\n", fn__stext, fn__etext);
    printk("\0016earc_route: HAL_A=%llx NONPCM=+%llu CS0=%llx CS2=%llx mode-select=%llx\n",
           layout.hal_a, layout.nonpcm_offset, layout.cs0, layout.cs2, layout.mode_select);
    printk("\0016earc_route: HAL_B=%llx rate=+%llu channels=%llx acmod=+%llu control=+%llu\n",
           layout.hal_b, layout.rate_offset, layout.channel_count, layout.acmod_offset, layout.control_offset);
    printk("\0016earc_route: BM mutex=%llx clock table=%llx stride=%llx entry3=%llx\n",
           layout.bm_mutex, layout.clock_table, layout.clock_stride, layout.clock_entry3);
}

static int apply(void) {
    int connected;
    void *current_source = (void *)0;
    u32 current_port = 0;
    __atomic_store_n(&hd_ready, 0, __ATOMIC_RELEASE);
    source_block = BM_GET("renderer", renderer);
    old_source_block = BM_GET(restore_pcm ? "mixer" : "encoder", restore_pcm ? 0 : 3);
    output_block = BM_GET("output", 3);
    if (!source_block || !old_source_block || !output_block) return -19;
    if (carrier_rate == 192000 && (output_clock(&original_clock_rate, &original_clock_flag) ||
        (original_clock_rate != 48000 && original_clock_rate != 192000))) return -19;
    /* renderer_get_uid_pcm_bypass_head deletes the renderer's pipeline and
     * builds a bypass one unless +76 (bypass pipeline) and +77 (PCM bypass)
     * are both set. Refuse rather than let the print do that. */
    if (!*((u8 *)source_block + 76) || !*((u8 *)source_block + 77)) {
        printk("\0013earc_route: refused; renderer%u has no PCM bypass pipeline\n", renderer);
        return -22;
    }
    printk("\0016earc_route: native PCM bypass head=%08x\n", PCM_HEAD(source_block));
    mutex_lock(BM_MUTEX);
    if (BM_GET("renderer", renderer) != source_block || BM_GET("output", 3) != output_block ||
        BM_GET(restore_pcm ? "mixer" : "encoder", restore_pcm ? 0 : 3) != old_source_block ||
        output_edge(&current_source, &current_port) != 1 || current_source != old_source_block || current_port != 0) {
        mutex_unlock(BM_MUTEX);
        printk("\0013earc_route: refused; the eARC output route changed during start\n");
        return -16;
    }
    original_nonpcm = WORD(nonpcm);
    original_rate = WORD(rate);
    if (original_nonpcm > 1 || (original_rate != 48000 && original_rate != 192000)) {
        mutex_unlock(BM_MUTEX);
        printk("\0013earc_route: refused; unexpected eARC baseline nonpcm=%u rate=%u\n", original_nonpcm, original_rate);
        return -22;
    }
    if (carrier_rate == 192000) {
        original_channels = WORD(channel_count);
        original_acmod = WORD(acmod);
        original_audio_mode = WORD(control) & 7;
        original_hal_mute = (WORD(cs2) >> 18) & 1;
        original_earc_on = *((u8 *)output_block + 129);
        if (*(u32 *)((u8 *)output_block + 44) != 3 || !*((u8 *)source_block + 77) ||
            (original_channels != 2 && original_channels != 6 && original_channels != 8) ||
            original_acmod > 7 || original_hal_mute != 0 || original_earc_on != 1) {
            mutex_unlock(BM_MUTEX);
            printk("\0013earc_route: refused HD baseline channels=%u acmod=%u mute=%u earc=%u\n",
                   original_channels, original_acmod, original_hal_mute, original_earc_on);
            return -22;
        }
        hd_saved = 1;
        HAL_MUTE(1);
        printk("\0016earc_route: HD baseline clock=%u flag=%u channels=%u acmod=%u mode=%u\n",
               original_clock_rate, original_clock_flag, original_channels, original_acmod, original_audio_mode);
    }
    applied = 1;
    DISCONNECT_LOCKED(output_block, 0);
    connected = CONNECT_LOCKED(source_block, 0x100, output_block, 0);
    mutex_unlock(BM_MUTEX);
    printk("\0016earc_route: native PCM carrier route result=%d\n", connected);
    if (connected < 0) { restore_route(); return connected; }
    HAL_FORMAT(1, carrier_rate);
    watchdog = kthread_create_on_node(watch_route, (void *)0, -1, "earc-guard");
    if ((u64)watchdog >= (u64)-4095) { watchdog = (void *)0; restore_route(); return -12; }
    wake_up_process(watchdog);
    printk("\0016earc_route: renderer%u PCM-bypass -> eARC at %u; 20-second producer lease\n", renderer, carrier_rate);
    return 0;
}

int init_module(void) {
    struct earc_code code[EF_COUNT];
    const char *refusal = check_parameters();
    unsigned int i;
    if (refusal) {
        printk("\0013earc_route: refused: %s\n", refusal);
        return -22;
    }
    for (i = 0; i < EF_COUNT; i++) {
        code[i].address = *functions[i].address;
        code[i].words = functions[i].size ? (const ed_u32 *)*functions[i].address : 0;
        code[i].count = functions[i].size ? *functions[i].size / 4 : 0;
    }
    refusal = earc_decode(code, &layout);
    if (refusal) {
        printk("\0013earc_route: refused: %s (at %llx)\n", refusal, ed_error_pc);
        return -8;
    }
    refusal = check_layout();
    if (refusal) {
        printk("\0013earc_route: refused: %s\n", refusal);
        return -8;
    }
    if (mode == 0) print_decoded();
    printk("\0016earc_route: decoded LG layout; renderer0=%px renderer1=%px output3=%px encoder3=%px mixer0=%px\n",
           BM_GET("renderer", 0), BM_GET("renderer", 1), BM_GET("output", 3), BM_GET("encoder", 3), BM_GET("mixer", 0));
    printk("\0016earc_route: initial eARC nonpcm=%u rate=%u channels=%u acmod=%u mode=%u CS0=%08x CS2=%08x\n",
           WORD(nonpcm), WORD(rate), WORD(channel_count), WORD(acmod), WORD(control) & 7, WORD(cs0), WORD(cs2));
    if (mode == 0) {
        audit();
        printk("\0016earc_route: audit complete; unloading\n");
        return -125;
    }
    return apply();
}

void cleanup_module(void) {
    if (watchdog) kthread_stop(watchdog);
    restore_route();
    printk("\0016earc_route: unloaded\n");
}

/* Linux's loader initialises the remaining fields. These offsets and the
 * complete 0x380-byte layout are those of the stock 5.4.268 modules
 * (aspectratiodrv.ko); build.py and the loader check them. */
struct target_module {
    u8 prefix[24];
    char name[56];
    u8 before_init[0x150 - 80];
    int (*initialise)(void);
    u8 before_exit[0x338 - 0x158];
    void (*finish)(void);
    u8 suffix[0x380 - 0x340];
};
_Static_assert(sizeof(struct target_module) == 0x380, "module size");
_Static_assert(__builtin_offsetof(struct target_module, initialise) == 0x150, "module init");
_Static_assert(__builtin_offsetof(struct target_module, finish) == 0x338, "module exit");
struct target_module __this_module __attribute__((section(".gnu.linkonce.this_module"), aligned(64), used)) = {
    .name = "earc_route", .initialise = init_module, .finish = cleanup_module
};

/* Linux 5.4 moduleparam.h: struct kernel_param_ops is {flags, set, get,
 * free}; struct kernel_param is three pointers, u16 perm, s8 level, u8 flags,
 * then the argument pointer. Permission 0 keeps the addresses out of sysfs. */
extern int param_set_uint(const char *value, const void *parameter);
extern int param_get_uint(char *value, const void *parameter);
extern int param_set_ulong(const char *value, const void *parameter);
extern int param_get_ulong(char *value, const void *parameter);
struct target_parameter_operations {
    unsigned int flags;
    int (*set)(const char *, const void *);
    int (*get)(char *, const void *);
    void (*free)(void *);
};
static const struct target_parameter_operations uint_operations = {0, param_set_uint, param_get_uint, (void *)0};
static const struct target_parameter_operations ulong_operations = {0, param_set_ulong, param_get_ulong, (void *)0};
struct target_parameter {
    const char *name;
    struct target_module *module;
    const void *operations;
    unsigned short permissions;
    signed char level;
    unsigned char flags;
    void *argument;
};
_Static_assert(sizeof(struct target_parameter) == 40, "parameter size");
#define EARC_SIZE_PARAMETER_1(name) {"sz_" #name, &__this_module, &uint_operations, 0, -1, 0, &sz_##name},
#define EARC_SIZE_PARAMETER_0(name)
#define EARC_PARAMETERS(name, analysed) \
    {"fn_" #name, &__this_module, &ulong_operations, 0, -1, 0, &fn_##name}, \
    EARC_SIZE_PARAMETER_##analysed(name)
static const struct target_parameter parameters[] __attribute__((section("__param"), aligned(8), used)) = {
    EARC_FUNCTIONS(EARC_PARAMETERS)
    {"fn__stext", &__this_module, &ulong_operations, 0, -1, 0, &fn__stext},
    {"fn__etext", &__this_module, &ulong_operations, 0, -1, 0, &fn__etext},
    {"mode", &__this_module, &uint_operations, 0400, -1, 0, &mode},
    {"renderer", &__this_module, &uint_operations, 0400, -1, 0, &renderer},
    {"lease", &__this_module, &uint_operations, 0600, -1, 0, &lease},
    {"active", &__this_module, &uint_operations, 0400, -1, 0, &applied},
    {"restore_pcm", &__this_module, &uint_operations, 0400, -1, 0, &restore_pcm},
    {"carrier_rate", &__this_module, &uint_operations, 0400, -1, 0, &carrier_rate},
    {"hd_ready", &__this_module, &uint_operations, 0400, -1, 0, &hd_ready}
};
static const char module_license[] __attribute__((section(".modinfo"), used)) = "license=GPL";
static const char module_vermagic[] __attribute__((section(".modinfo"), used)) = "vermagic=5.4.268 SMP preempt mod_unload aarch64";
static const char module_description[] __attribute__((section(".modinfo"), used)) =
    "description=Self-adapting port of Glasshouse's native DTS eARC forwarding route (5.4.268)";
__asm__(".pushsection .plt,\"ax\",%progbits\n.byte 0\n.popsection\n"
        ".pushsection .init.plt,\"a\",%progbits\n.byte 0\n.popsection\n"
        ".pushsection .text.ftrace_trampoline,\"ax\",%progbits\n.byte 0\n.popsection\n");
