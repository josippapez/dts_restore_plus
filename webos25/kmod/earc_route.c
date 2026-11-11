/* SPDX-License-Identifier: GPL-2.0-only */
/* eARC route for rooted LG webOS 25 TVs, on the 5.4.268 and 6.12.44 kernel
 * generations (one source, two builds: see the EARC_* macros below).
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
/* Per-generation build parameters, set by build.py with -D; the defaults are
 * the 5.4.268 generation. EARC_MODULE_SIZE, EARC_MODULE_INIT and EARC_MODULE_EXIT
 * are sizeof(struct module) and the offsets of its init and exit pointers in the
 * stock aspectratiodrv.ko of that generation; EARC_PRINTK_SYMBOL is the kernel's
 * name for printk (_printk on 6.12). */
#ifndef EARC_MODULE_SIZE
#define EARC_MODULE_SIZE 0x380
#define EARC_MODULE_INIT 0x150
#define EARC_MODULE_EXIT 0x338
#endif
#ifndef EARC_VERMAGIC
#define EARC_VERMAGIC "5.4.268 SMP preempt mod_unload aarch64"
#endif
#ifndef EARC_PRINTK_SYMBOL
#define EARC_PRINTK_SYMBOL "printk"
#endif
/* struct kprobe's size and the offset of its last field (flags), per generation,
 * measured from register_kprobe/arch_prepare_kprobe/kprobe_breakpoint_handler in
 * the captures (5.4: flags +0x80, size 0x88; 6.12: flags +0x78, size 0x80 after
 * fault_handler was dropped). The kernel touches nothing past flags, so this is
 * the full size. The active modes (mode>=2) are the only users. Defaults: 5.4. */
#ifndef EARC_KPROBE_SIZE
#define EARC_KPROBE_SIZE 0x88
#define EARC_KPROBE_FLAGS 0x80
#endif
#include "earc_decode.h"
#include "earc_hdmi.h"
#include "earc_probe.h"

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef void *(*block_fn)(const char *, unsigned int);
typedef int (*connection_fn)(void *, unsigned int, void *, unsigned int);
typedef int (*disconnect_fn)(void *, unsigned int);
typedef void (*format_fn)(unsigned int, unsigned int);
extern int printk(const char *format, ...) __asm__(EARC_PRINTK_SYMBOL);
extern void *kthread_create_on_node(int (*function)(void *), void *data, int node, const char *name, ...);
extern int wake_up_process(void *task);
extern int kthread_stop(void *task);
extern _Bool kthread_should_stop(void);
extern void msleep(unsigned int milliseconds);
extern void mutex_lock(void *mutex);
extern void mutex_unlock(void *mutex);
/* Exported on every 5.4 and 6.12 kernel measured (build.py proves it); the
 * active modes (mode>=2) call them directly. register_kernel_break_hook,
 * unregister_kernel_break_hook and aarch64_insn_patch_text are NOT exported and
 * are called through their kallsyms addresses (fn_ below). */
extern int register_kprobe(void *kp);
extern void unregister_kprobe(void *kp);
extern void cpus_read_lock(void);
extern void cpus_read_unlock(void);
extern void synchronize_rcu(void);

/* Kernel structs declared by hand, per generation, like struct target_module.
 * struct pt_regs and struct break_hook are identical across both generations;
 * only struct kprobe's size and last-field offset differ (the EARC_KPROBE_*
 * macros, set by build.py). We set only addr and pre_handler in a kprobe and
 * only fn/imm/mask in a break_hook; the rest the kernel fills or we leave zero. */
struct target_pt_regs { u64 regs[31]; };  /* x0..x30 at +0..+240; we touch x0..x3 */
_Static_assert(__builtin_offsetof(struct target_pt_regs, regs) == 0, "pt_regs regs");
struct target_kprobe;
typedef int (*prehandler_fn)(struct target_kprobe *, struct target_pt_regs *);
struct target_kprobe {
    u8 before_addr[0x28];
    void *addr;                 /* +0x28 */
    const char *symbol_name;    /* +0x30 */
    u32 offset;                 /* +0x38 */
    u8 pad_3c[4];
    prehandler_fn pre_handler;  /* +0x40 */
    u8 tail[EARC_KPROBE_SIZE - 0x48];
};
_Static_assert(sizeof(struct target_kprobe) == EARC_KPROBE_SIZE, "kprobe size");
_Static_assert(__builtin_offsetof(struct target_kprobe, addr) == 0x28, "kprobe addr");
_Static_assert(__builtin_offsetof(struct target_kprobe, offset) == 0x38, "kprobe offset");
_Static_assert(__builtin_offsetof(struct target_kprobe, pre_handler) == 0x40, "kprobe pre_handler");
_Static_assert(EARC_KPROBE_FLAGS + 4 <= EARC_KPROBE_SIZE, "kprobe flags within size");
struct target_break_hook {
    void *next;   /* +0 */
    void *prev;   /* +8 */
    void *fn;            /* +16 */
    unsigned short imm;  /* +24 */
    unsigned short mask; /* +26 */
    u8 pad[4];
};
_Static_assert(sizeof(struct target_break_hook) == 32, "break_hook size");
_Static_assert(__builtin_offsetof(struct target_break_hook, fn) == 16, "break_hook fn");
_Static_assert(__builtin_offsetof(struct target_break_hook, imm) == 24, "break_hook imm");
_Static_assert(__builtin_offsetof(struct target_break_hook, mask) == 26, "break_hook mask");

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
/* HDMI passthrough (mode>=2) state; declared here because check_parameters and
 * the parameter table reference some of it. The logic is far below in the
 * "HDMI passthrough active modes" section. */
static struct target_kprobe route_probe, selftest_probe;
static struct target_break_hook hook_probe, hook_step;  /* BRK imm 4 and imm 6 */
static u32 hooks_registered, probe_registered, gate_open, gate_opened_by_us, selftest_passed;
static ed_u32 gate_written;
static u32 calls_seen, calls_matched, overrides, match_prints;
static u32 window_seconds = 30, supervised, window_open = 1;
static int override_mode;          /* mode 5: override; mode 4: observe only */
static u32 selftest_hits;
static void *window_task;

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

/* HDMI-passthrough audit set (optional): the loader passes fn_/sz_ for each.
 * These are read and printed in the audit only; none is called, and the service
 * functions (patch_text, kprobe, cpu locks) are addresses only. A missing or
 * invalid HDMI symbol disables only the HDMI audit, never the route. */
#define EARC_HDMI_ADDRESS(name, analysed) static unsigned long fn_##name;
EARC_HDMI_FUNCTIONS(EARC_HDMI_ADDRESS)
#undef EARC_HDMI_ADDRESS
#define EARC_HDMI_SIZE_1(name) static unsigned int sz_##name;
#define EARC_HDMI_SIZE_0(name)
#define EARC_HDMI_SIZE(name, analysed) EARC_HDMI_SIZE_##analysed(name)
EARC_HDMI_FUNCTIONS(EARC_HDMI_SIZE)
#undef EARC_HDMI_SIZE
static const struct {
    const char *name;
    unsigned long *address;
    unsigned int *size;
} hdmi_functions[HF_COUNT] = {
#define EARC_HDMI_SIZE_POINTER_1(name) &sz_##name
#define EARC_HDMI_SIZE_POINTER_0(name) 0
#define EARC_HDMI_ROW(name, analysed) {#name, &fn_##name, EARC_HDMI_SIZE_POINTER_##analysed(name)},
    EARC_HDMI_FUNCTIONS(EARC_HDMI_ROW)
#undef EARC_HDMI_ROW
};
static struct earc_hdmi_layout hdmi;

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
 * earc_decode derived in aud_clock_update_output: entry[8] says its mutex is
 * initialised, entry[9] that the output is enabled; the object at +16 holds
 * the rate, the channel count (6.12 only), the flag and the control at +128,
 * whose clock_callback_offset is the callback. */
static int output_clock(u32 *rate, u32 *flag, u32 *channels) {
    u8 *entry = (u8 *)layout.clock_entry3, *object, *control;
    int result = -19;
    if (!entry[8]) return result;
    mutex_lock(entry + layout.clock_mutex_offset);
    object = *(u8 **)(entry + 16);
    if (entry[9] && object) {
        control = *(u8 **)(object + 128);
        if (control && *(void **)(control + layout.clock_callback_offset) == CLOCK_CALLBACK) {
            *rate = *(u32 *)(object + layout.clock_object_rate);
            *flag = object[layout.clock_object_flag];
            if (layout.clock_takes_channels) *channels = *(u32 *)(object + layout.clock_object_channels);
            if (valid_output_rate(*rate) && *flag <= 1 &&
                (!layout.clock_takes_channels || (*channels >= 1 && *channels <= 8)))
                result = 0;
        }
    }
    mutex_unlock(entry + layout.clock_mutex_offset);
    return result;
}

/* aud_clock_update_output(3, rate, flag) on 5.4, (3, rate, channels, flag) on
 * 6.12, as earc_decode found its signature. */
static int update_clock(u32 rate, u32 channels, u32 flag) {
    if (layout.clock_takes_channels)
        return ((int (*)(u32, u32, u32, u32))fn_aud_clock_update_output)(3, rate, channels, flag);
    return ((int (*)(u32, u32, u32))fn_aud_clock_update_output)(3, rate, flag);
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
static u32 original_clock_rate, original_clock_flag, original_clock_channels, original_channels, original_acmod;
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
    u32 rate = 0, flag = 0, clock_channels = 0;
    return !output_clock(&rate, &flag, &clock_channels) && rate == 192000 && flag == original_clock_flag &&
        (!layout.clock_takes_channels || clock_channels == 8) &&
        WORD(nonpcm) == 1 && WORD(rate) == 192000 && WORD(channel_count) == 8 &&
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
    result = update_clock(192000, 8, original_clock_flag);
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
    u32 rate = 0, flag = 0, clock_channels = 0;
    unsigned int attempt;
    int foreign = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        if (!foreign_output_state(&state, &foreign) || output_clock(&rate, &flag, &clock_channels)) {
            msleep(50);
            continue;
        }
        update_clock(state.rate, state.channels, state.flag);
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
        u32 rate = 0, flag = 0, clock_channels = 0;
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
        result = update_clock(original_clock_rate, original_clock_channels, original_clock_flag);
        HAL_MUTE(original_hal_mute);
        HAL_SET_ON(original_earc_on);
        msleep(100);
        printk("\0016earc_route: HD restore clock=%u flag=%u result=%d readback=%d\n",
               original_clock_rate, original_clock_flag, result,
               !output_clock(&rate, &flag, &clock_channels) && rate == original_clock_rate && flag == original_clock_flag);
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
            u32 rate = 0, flag = 0, clock_channels = 0;
            HAL_MUTE(1);
            __atomic_store_n(&hd_ready, 0, __ATOMIC_RELEASE);
            output_clock(&rate, &flag, &clock_channels);
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
    u32 rate = 0, flag = 0, port = 0, clock_channels = 0;
    unsigned int index;
    int clock = output_clock(&rate, &flag, &clock_channels), edge;
    printk("\0016earc_route: HAL mode-select=%u (1 selects mode 7, not 6)\n", WORD(mode_select));
    printk("\0016earc_route: clock3 ready=%u enabled=%u arc-callback=%d result=%d rate=%u channels=%u flag=%u\n",
           entry[8], entry[9], *(void **)(entry + layout.clock_callback_offset) == CLOCK_CALLBACK, clock, rate,
           clock_channels, flag);
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
    if (mode > 5 || renderer > 1 || restore_pcm > 1 || (carrier_rate != 48000 && carrier_rate != 192000))
        return "mode, renderer, restore_pcm or carrier_rate out of range";
    if (window_seconds > 120)
        return "window_seconds out of range (max 120)";
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
    printk("\0016earc_route: clock mutex=+%llx object rate=+%llu channels=+%llu (taken=%llu) flag=+%llu callback=+%llu channel callback=+%llu\n",
           layout.clock_mutex_offset, layout.clock_object_rate, layout.clock_object_channels,
           layout.clock_takes_channels, layout.clock_object_flag, layout.clock_callback_offset,
           layout.clock_channel_callback_offset);
}

/* A derived HDMI data address must be aligned and lie in the kernel's data,
 * the 64 MiB after _etext, as check_layout requires of the others, before
 * anything is read through it. size is how many bytes are read there. */
#define HDMI_DATA_SPAN 0x4000000UL
static int hdmi_addr_ok(unsigned long address, unsigned long align, unsigned long size) {
    return !(address & (align - 1)) && address >= fn__etext && address - fn__etext < HDMI_DATA_SPAN &&
        HDMI_DATA_SPAN - (address - fn__etext) >= size;
}

/* Count consecutive table entries that are pointers to a printable C string of
 * fewer than 80 bytes, bounds-checked, capped. Read-only. */
static unsigned int hdmi_name_entries(unsigned long table) {
    unsigned int count = 0, i;
    while (count < 32 && hdmi_addr_ok(table + 8UL * count, 8, 8)) {
        unsigned long p = *(volatile unsigned long *)(table + 8UL * count);
        const volatile u8 *s = (const volatile u8 *)p;
        if (!hdmi_addr_ok(p, 1, 80)) break;
        for (i = 0; i < 80 && s[i] >= 32 && s[i] < 127; i++) {}
        if (!i || i >= 80 || s[i] != 0) break;
        count++;
    }
    return count;
}

/* The break-hook walk's memory read. earc_hdmi_walk_list only passes addresses
 * inside the bound it was given, the same one hdmi_addr_ok checks. */
static int hdmi_read(void *context, ed_u64 address, unsigned int width, ed_u64 *value) {
    (void)context;
    *value = width == 8 ? *(volatile unsigned long *)address : *(volatile u32 *)address;
    return 1;
}

static const char *gate_state_name(int state) {
    return state == GATE_STOCK ? "stock(returns 0)" : state == GATE_SWITCHED ? "switched(returns 1; another tool active)" :
        state == GATE_BREAKPOINT ? "breakpoint(another tool is probing it)" : "unknown";
}

/* Read-only HDMI-passthrough audit. Derives the HDMI set from LG's code and
 * prints it; reads a few live data words (ADEC 1's state, the codec-name
 * table, the kernel-image nodes of the break-hook list) through the derived,
 * bounds-checked addresses. Writes nothing, calls nothing new, imports nothing.
 * A missing symbol or unmatched pattern is reported, never a refusal. */
static void hdmi_audit(void) {
    static struct earc_hdmi_walk walk;
    struct earc_code code[HF_COUNT];
    unsigned int i, missing = 0;
    for (i = 0; i < HF_COUNT; i++) {
        unsigned long address = *hdmi_functions[i].address;
        unsigned int size = hdmi_functions[i].size ? *hdmi_functions[i].size : 0;
        int ok = earc_hdmi_valid(address, size, hdmi_functions[i].size != 0, fn__stext, fn__etext);
        code[i].address = ok ? address : 0;
        code[i].words = (ok && hdmi_functions[i].size) ? (const ed_u32 *)address : 0;
        code[i].count = (ok && hdmi_functions[i].size) ? size / 4 : 0;
        if (ok || (i == HF_kretprobe_breakpoint_handler && !address)) continue;  /* kretprobe handler: 6.12 only */
        missing++;
        printk("\0016earc_route: hdmi symbol %s missing or out of range (%lx size %u)\n", hdmi_functions[i].name, address, size);
    }
    earc_hdmi_decode(code, layout.nonpcm, fn__stext & ~0xfffffUL, &hdmi);

    printk("\0016earc_route: hdmi gate %s addr=%llx words=%08x %08x\n",
           gate_state_name(hdmi.gate_state), hdmi.gate, hdmi.gate_words[0], hdmi.gate_words[1]);
    printk("\0016earc_route: hdmi service patch_text=%llx register_kprobe=%llx unregister_kprobe=%llx cpus_read_lock=%llx cpus_read_unlock=%llx\n",
           hdmi.patch_text, hdmi.kprobe_register, hdmi.kprobe_unregister, hdmi.cpus_read_lock, hdmi.cpus_read_unlock);
    if (!hdmi.set_output_reason)
        printk("\0016earc_route: hdmi set_output=%llx first=%08x mask-offset=%llu (0x20 means eARC)\n",
               hdmi.set_output, hdmi.set_output_first_word, hdmi.mask_offset);
    else
        printk("\0016earc_route: hdmi set_output not available: %s (addr=%llx first=%08x)\n",
               hdmi.set_output_reason, hdmi.set_output, hdmi.set_output_first_word);
    if (!hdmi.adec_reason) {
        printk("\0016earc_route: hdmi adec table=%llx stride=%llu codec=+%llu open=+%llu started=+%llu\n",
               hdmi.adec_base, hdmi.adec_stride, hdmi.adec_codec_off, hdmi.adec_open_off, hdmi.adec_started_off);
        if (hdmi_addr_ok(hdmi.adec1_codec, 4, 4) && hdmi_addr_ok(hdmi.adec1_open, 1, 1) && hdmi_addr_ok(hdmi.adec1_started, 1, 1))
            printk("\0016earc_route: hdmi adec1 codec@%llx=%u open@%llx=%u started@%llx=%u\n",
                   hdmi.adec1_codec, *(volatile u32 *)hdmi.adec1_codec, hdmi.adec1_open, *(volatile u8 *)hdmi.adec1_open,
                   hdmi.adec1_started, *(volatile u8 *)hdmi.adec1_started);
        else
            printk("\0013earc_route: hdmi adec1 address out of range codec=%llx open=%llx started=%llx\n",
                   hdmi.adec1_codec, hdmi.adec1_open, hdmi.adec1_started);
    } else {
        printk("\0016earc_route: hdmi adec not available: %s\n", hdmi.adec_reason);
    }
    if (!hdmi.cb_reason) {
        printk("\0016earc_route: hdmi cb_output_status=%llx shape=%d (%s) hd-compare=%llx bound=%llu hd-supported=%llx\n",
               hdmi.cb_output_status, hdmi.hd_shape, hdmi.hd_shape == 1 ? "cmp; b.ls to supported" : "cmp; b.ne falls into supported",
               hdmi.hd_compare, hdmi.hd_compare_bound, hdmi.hd_supported_branch);
        printk("\0016earc_route: hdmi cb name-load=%llx table=%llx entries=%u\n",
               hdmi.hd_name_load, hdmi.name_table, hdmi_name_entries(hdmi.name_table));
    } else if (hdmi.hd_brk_count) {
        printk("\0016earc_route: hdmi cb_output_status=%llx not available: %s: %u breakpoint(s), first at +%llu\n",
               hdmi.cb_output_status, hdmi.cb_reason, hdmi.hd_brk_count, hdmi.hd_brk_offset);
    } else {
        printk("\0016earc_route: hdmi cb_output_status=%llx not available: %s\n", hdmi.cb_output_status, hdmi.cb_reason);
    }
    if (!hdmi.break_reason) {
        /* imm and mask named only if they are the two halves of the word after fn. */
        int fields = hdmi.break_fields_known && hdmi.break_imm_offset == hdmi.break_word_offset &&
            hdmi.break_mask_offset == hdmi.break_word_offset + 2;
        earc_hdmi_walk_list(&hdmi, fn__etext, fn__etext + HDMI_DATA_SPAN, hdmi_read, (void *)0, &walk);
        printk("\0016earc_route: hdmi break list=%llx register=%llx unregister=%llx fn=+%llu imm=+%llu mask=+%llu\n",
               hdmi.break_list_head, hdmi.break_register, hdmi.break_unregister, hdmi.break_fn_offset,
               hdmi.break_imm_offset, hdmi.break_mask_offset);
        printk("\0016earc_route: hdmi break handlers kprobe=%llx kprobe-ss=%llx kretprobe=%llx\n",
               hdmi.break_fn, hdmi.step_fn, hdmi.kret_fn);
        for (i = 0; i < walk.count; i++) {
            if (fields)
                printk("\0016earc_route: hdmi break node%u=%llx fn=%llx imm=%u mask=%x\n", i, walk.nodes[i].address,
                       walk.nodes[i].fn, walk.nodes[i].word & 0xffff, walk.nodes[i].word >> 16);
            else
                printk("\0016earc_route: hdmi break node%u=%llx fn=%llx word-after-fn=%08x\n", i, walk.nodes[i].address,
                       walk.nodes[i].fn, walk.nodes[i].word);
        }
        if (walk.status == HW_OUTSIDE && walk.stop == hdmi.break_list_head)
            printk("\0013earc_route: hdmi break list head %llx is outside the kernel image\n", walk.stop);
        else if (walk.status == HW_OUTSIDE)
            printk("\0016earc_route: hdmi break list continues outside the kernel image at %llx\n", walk.stop);
        else if (walk.status != HW_COMPLETE)
            printk("\0016earc_route: hdmi break walk stopped before %llx\n", walk.stop);
        printk("\0016earc_route: hdmi break walk %s nodes=%u kprobe=%d kprobe-ss=%d kretprobe=%d\n",
               walk.status == HW_COMPLETE ? "complete" : walk.status == HW_OUTSIDE ? "incomplete(leaves image)" :
               walk.status == HW_MISALIGNED ? "incomplete(misaligned next)" :
               walk.status == HW_CAPPED ? "incomplete(more than 32 nodes)" : "incomplete(unreadable)",
               walk.count, walk.saw_break, walk.saw_step, walk.saw_kret);
    } else {
        printk("\0016earc_route: hdmi break hooks not available: %s\n", hdmi.break_reason);
    }
    printk("\0016earc_route: hdmi nonpcm virtual=%llx physical=%llx\n", hdmi.nonpcm_virtual, hdmi.nonpcm_physical);
    if (missing)
        printk("\0016earc_route: hdmi passthrough: not available: %u symbol(s) missing\n", missing);
}

/* ---- HDMI passthrough active modes (mode>=2) ----
 * mode 2 self-tests the break handlers and a kprobe, then unloads. mode 3 opens
 * the DTS gate and stays loaded. mode 4 opens the gate and observes _set_output
 * calls. mode 5 does the same and overrides the DTS-core arguments for a bounded
 * window. Every kernel write is listed with its undo in the README; a failure
 * after a partial setup undoes it in reverse. These paths never run for mode 0
 * or mode 1 and add nothing to the route's code path.
 *
 * The non-exported service functions are called through their kallsyms addresses
 * (validated in [_stext,_etext) by the loader and earc_hdmi_valid). */
#define REGISTER_BREAK_HOOK ((void (*)(void *))fn_register_kernel_break_hook)
#define UNREGISTER_BREAK_HOOK ((void (*)(void *))fn_unregister_kernel_break_hook)
#define PATCH_TEXT ((int (*)(void **, u32 *, int))fn_aarch64_insn_patch_text)

/* A leaf function the self-test probes; never called except through this
 * volatile pointer, so -O2 cannot fold the call or inline it past the probe.
 * It must stay a named symbol in the module's symbol table: on 6.12
 * register_kprobe returns -ENOENT unless kallsyms resolves the probed address,
 * so stripping the module would break mode 2 here. */
static __attribute__((noinline)) int selftest_fn(int x) { return x + 1; }
static int (*volatile selftest_call)(int) = selftest_fn;

/* Runs in breakpoint context (BRK #4 handler). Reads only the derived,
 * load-time bounds-checked ADEC addresses and the caller's descriptor (null
 * checked), updates atomics, and for mode 5 rewrites x2/x3 within the window.
 * It calls no kernel function and we only ever probe _set_output and the
 * self-test leaf, never this handler, so no probe recursion is possible. */
static int route_prehandler(struct target_kprobe *p, struct target_pt_regs *regs) {
    ed_u64 descriptor = regs->regs[1];
    ed_u32 mask_word, codec;
    ed_u8 open, started;
    (void)p;
    __atomic_add_fetch(&calls_seen, 1, __ATOMIC_RELAXED);
    if (!descriptor) return 0;
    mask_word = *(volatile u32 *)(descriptor + hdmi.mask_offset);
    codec = *(volatile u32 *)hdmi.adec1_codec;
    open = *(volatile u8 *)hdmi.adec1_open;
    started = *(volatile u8 *)hdmi.adec1_started;
    if (!earc_policy_match(regs->regs[0], mask_word, codec, open, started)) return 0;
    __atomic_add_fetch(&calls_matched, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&match_prints, 1, __ATOMIC_RELAXED) <= 5)
        printk("\0016earc_route: pass match index=%llu codec-arg=%u bypass-arg=%u adec1-codec=%u\n",
               (unsigned long long)regs->regs[0], (u32)regs->regs[2], (u32)regs->regs[3], codec);
    if (override_mode && __atomic_load_n(&window_open, __ATOMIC_ACQUIRE)) {
        regs->regs[2] = 0;  /* codec -> 0 */
        regs->regs[3] = 1;  /* bypass -> 1 */
        __atomic_add_fetch(&overrides, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static int selftest_prehandler(struct target_kprobe *p, struct target_pt_regs *regs) {
    (void)p;
    regs->regs[0] = 10;
    __atomic_add_fetch(&selftest_hits, 1, __ATOMIC_RELAXED);
    return 0;  /* 0: single-step the original add w0,w0,#1 -> 11 */
}

/* Registers module-owned break hooks for BRK #4 (kprobe) and #6 (single-step),
 * pointing at the kernel's own handlers. mask 0: each answers only its own
 * immediate. The caller has decided (earc_hook_decide) that registration is
 * needed and conflict-free. */
static void hooks_register(void) {
    hook_probe.fn = (void *)hdmi.break_fn;
    hook_probe.imm = EARC_BRK_IMM_PROBE;
    hook_probe.mask = 0;
    hook_step.fn = (void *)hdmi.step_fn;
    hook_step.imm = EARC_BRK_IMM_STEP;
    hook_step.mask = 0;
    REGISTER_BREAK_HOOK(&hook_probe);
    REGISTER_BREAK_HOOK(&hook_step);
    hooks_registered = 1;
}
static void hooks_unregister(void) {
    if (!hooks_registered) return;
    UNREGISTER_BREAK_HOOK(&hook_step);
    UNREGISTER_BREAK_HOOK(&hook_probe);
    hooks_registered = 0;
}
/* Callers follow hooks_unregister (and unregister_kprobe) with an explicit
 * synchronize_rcu. That is a second, redundant wait: unregister_kernel_break_hook
 * calls synchronize_rcu itself (both generations), and unregister_kprobe does so
 * through unregister_kprobes. We keep it, as lg-g5-dts does, but it is a backstop,
 * not the only grace period. */

/* Stage a: walk the kernel's break-hook list and act. Returns 0 (hooks present,
 * whether the kernel's or newly ours), or a negative refusal. */
static struct earc_hdmi_walk pt_walk;
static int stage_a_hooks(void) {
    enum earc_hook_decision decision;
    earc_hdmi_walk_list(&hdmi, fn__etext, fn__etext + HDMI_DATA_SPAN, hdmi_read, (void *)0, &pt_walk);
    decision = earc_hook_decide(&pt_walk, hdmi.break_fields_known);
    switch (decision) {
    case EHD_ALREADY:
        printk("\0016earc_route: break hooks already on the list (kernel owns them)\n");
        return 0;
    case EHD_REGISTER:
        hooks_register();
        printk("\0016earc_route: registered module break hooks #%u=%llx #%u=%llx\n",
               EARC_BRK_IMM_PROBE, hdmi.break_fn, EARC_BRK_IMM_STEP, hdmi.step_fn);
        return 0;
    case EHD_INCOMPLETE:
        printk("\0013earc_route: refused; break-hook walk incomplete (status=%d)\n", pt_walk.status);
        return -16;
    case EHD_CONFLICT:
    default:
        printk("\0013earc_route: refused; a break hook already answers BRK #%u or #%u\n",
               EARC_BRK_IMM_PROBE, EARC_BRK_IMM_STEP);
        return -16;
    }
}

/* The kprobe self-test. Must run only after stage a: an unhandled BRK would
 * panic. Registers a probe on the leaf, calls it, and checks the pre-handler
 * ran exactly once and changed the result. */
static int selftest(void) {
    int result;
    selftest_hits = 0;
    selftest_probe.addr = (void *)selftest_fn;
    selftest_probe.pre_handler = selftest_prehandler;
    if (register_kprobe(&selftest_probe) < 0) {
        printk("\0013earc_route: kprobe self-test: register_kprobe failed\n");
        selftest_passed = 0;
        return -1;
    }
    result = selftest_call(7);
    unregister_kprobe(&selftest_probe);
    selftest_passed = result == 11 && selftest_hits == 1;
    printk("\0016earc_route: kprobe self-test result=%d (want 11) hits=%u (want 1): %s\n",
           result, selftest_hits, selftest_passed ? "ok" : "FAILED");
    return selftest_passed ? 0 : -1;
}

/* Opens the DTS gate: patch its first word mov w0,#0 -> mov w0,#1, under
 * cpus_read_lock, re-checking the stock words beneath the lock. */
static int gate_open_patch(void) {
    void *addrs[1];
    u32 insns[1];
    ed_u32 w0, w1;
    int result;
    if ((hdmi.gate & 3) || hdmi.gate < fn__stext || hdmi.gate >= fn__etext) {
        printk("\0013earc_route: refused; DTS gate %llx outside kernel text\n", hdmi.gate);
        return -22;
    }
    cpus_read_lock();
    w0 = *(volatile u32 *)hdmi.gate;
    w1 = *(volatile u32 *)(hdmi.gate + 4);
    if (w0 != EARC_GATE_STOCK_W0 || w1 != 0xd65f03c0u) {
        cpus_read_unlock();
        printk("\0013earc_route: refused; DTS gate not stock under lock (%08x %08x)\n", w0, w1);
        return -22;
    }
    addrs[0] = (void *)hdmi.gate;
    insns[0] = EARC_GATE_OPEN_W0;
    result = PATCH_TEXT(addrs, insns, 1);
    cpus_read_unlock();
    if (result < 0) {
        printk("\0013earc_route: DTS gate patch failed result=%d\n", result);
        return result;
    }
    gate_written = EARC_GATE_OPEN_W0;
    gate_opened_by_us = 1;
    gate_open = 1;
    printk("\0016earc_route: DTS gate opened; words now %08x %08x\n",
           *(volatile u32 *)hdmi.gate, *(volatile u32 *)(hdmi.gate + 4));
    return 0;
}

/* Restores the gate, but only if its first word is still the one we wrote. */
static void gate_restore(void) {
    void *addrs[1];
    u32 insns[1];
    ed_u32 current, want;
    int result;
    if (!gate_opened_by_us) return;
    cpus_read_lock();
    current = *(volatile u32 *)hdmi.gate;
    want = earc_gate_restore_word(current, gate_written);
    if (!want) {
        cpus_read_unlock();
        printk("\0013earc_route: DTS gate first word %08x is not ours; leaving it\n", current);
        gate_opened_by_us = 0;
        gate_open = current == EARC_GATE_OPEN_W0;
        return;
    }
    addrs[0] = (void *)hdmi.gate;
    insns[0] = want;
    result = PATCH_TEXT(addrs, insns, 1);
    cpus_read_unlock();
    gate_opened_by_us = 0;
    gate_open = (*(volatile u32 *)hdmi.gate) == EARC_GATE_OPEN_W0;
    printk("\0016earc_route: DTS gate restored result=%d words now %08x %08x\n",
           result, *(volatile u32 *)hdmi.gate, *(volatile u32 *)(hdmi.gate + 4));
}

/* The override window, driven by the watchdog-thread pattern (no new import for
 * a clock). After window_seconds it stops overriding; supervised with
 * window_seconds 0 never expires. */
static int window_thread(void *unused) {
    unsigned int ticks = 0, limit = window_seconds * 10;
    (void)unused;
    if (window_seconds)
        while (ticks < limit && !kthread_should_stop()) { msleep(100); ticks++; }
    if (ticks >= limit && window_seconds) {
        __atomic_store_n(&window_open, 0, __ATOMIC_RELEASE);
        printk("\0016earc_route: override window of %u s elapsed; observing only\n", window_seconds);
    }
    while (!kthread_should_stop()) msleep(100);
    return 0;
}

/* Builds the HDMI code array and derives the set into the global `hdmi`, as the
 * audit does (kept separate so mode 0 is byte-identical). */
static void passthrough_derive(void) {
    struct earc_code code[HF_COUNT];
    unsigned int i;
    for (i = 0; i < HF_COUNT; i++) {
        unsigned long address = *hdmi_functions[i].address;
        unsigned int size = hdmi_functions[i].size ? *hdmi_functions[i].size : 0;
        int ok = earc_hdmi_valid(address, size, hdmi_functions[i].size != 0, fn__stext, fn__etext);
        code[i].address = ok ? address : 0;
        code[i].words = (ok && hdmi_functions[i].size) ? (const ed_u32 *)address : 0;
        code[i].count = (ok && hdmi_functions[i].size) ? size / 4 : 0;
    }
    earc_hdmi_decode(code, layout.nonpcm, fn__stext & ~0xfffffUL, &hdmi);
}

static void passthrough_print_walk(const char *when) {
    earc_hdmi_walk_list(&hdmi, fn__etext, fn__etext + HDMI_DATA_SPAN, hdmi_read, (void *)0, &pt_walk);
    printk("\0016earc_route: break walk %s: %s nodes=%u kprobe=%d kprobe-ss=%d\n", when,
           pt_walk.status == HW_COMPLETE ? "complete" : "incomplete", pt_walk.count,
           pt_walk.saw_break, pt_walk.saw_step);
}

/* mode 2: register hooks (or confirm the kernel's), self-test, undo, unload.
 * Everything is always undone before returning. The return code distinguishes
 * the outcomes so the loader does not report a refusal or a failure as success:
 * -ECANCELED only when the self-test passed, the stage-a refusal code when it
 * refuses, -EIO when the self-test failed. */
static int selftest_mode(void) {
    int registered, refusal = stage_a_hooks(), ok;
    if (refusal < 0) { passthrough_print_walk("after refusal"); return refusal; }
    registered = hooks_registered;
    ok = selftest() == 0;
    passthrough_print_walk("with hooks");
    hooks_unregister();
    if (registered) synchronize_rcu();
    passthrough_print_walk("after undo");
    if (ok) {
        printk("\0016earc_route: self-test passed; unloading\n");
        return -125;
    }
    printk("\0013earc_route: self-test FAILED; unloading\n");
    return -5;
}

/* mode 3: open the gate and stay loaded (cleanup restores it). */
static int gate_mode(void) {
    if (gate_open_patch() < 0) return -22;
    printk("\0016earc_route: DTS gate open (mode 3); unload to restore\n");
    return 0;
}

/* modes 4 (observe) and 5 (route override). One load: hooks + gate + probe. */
static int session_mode(int override) {
    if (!hdmi_addr_ok(hdmi.adec1_codec, 4, 4) || !hdmi_addr_ok(hdmi.adec1_open, 1, 1) ||
        !hdmi_addr_ok(hdmi.adec1_started, 1, 1)) {
        printk("\0013earc_route: refused; ADEC1 address out of range codec=%llx open=%llx started=%llx\n",
               hdmi.adec1_codec, hdmi.adec1_open, hdmi.adec1_started);
        return -22;
    }
    if ((hdmi.set_output & 3) || hdmi.set_output < fn__stext || hdmi.set_output >= fn__etext) {
        printk("\0013earc_route: refused; _set_output %llx outside kernel text\n", hdmi.set_output);
        return -22;
    }
    if (override && !window_seconds && !supervised) {
        printk("\0013earc_route: refused; window_seconds=0 needs supervised=1\n");
        return -22;
    }
    if (stage_a_hooks() < 0) return -16;
    if (selftest() < 0) { hooks_unregister(); synchronize_rcu(); return -16; }
    if (gate_open_patch() < 0) { hooks_unregister(); synchronize_rcu(); return -22; }
    override_mode = override;
    route_probe.addr = (void *)hdmi.set_output;
    route_probe.pre_handler = route_prehandler;
    if (register_kprobe(&route_probe) < 0) {
        printk("\0013earc_route: register_kprobe on _set_output failed\n");
        gate_restore();
        hooks_unregister();
        synchronize_rcu();
        return -16;
    }
    probe_registered = 1;
    if (override) {
        window_task = kthread_create_on_node(window_thread, (void *)0, -1, "earc-dts-window");
        if ((u64)window_task >= (u64)-4095) {
            window_task = (void *)0;
            unregister_kprobe(&route_probe);
            probe_registered = 0;
            gate_restore();
            hooks_unregister();
            synchronize_rcu();
            return -12;
        }
        wake_up_process(window_task);
    }
    printk("\0016earc_route: HDMI passthrough %s active on _set_output=%llx; gate open%s\n",
           override ? "route(override)" : "observe", hdmi.set_output,
           override ? "; override window running" : "");
    printk("\0013earc_route: UNTESTED ON HARDWARE: this mode has never run on a TV\n");
    return 0;
}

/* Tears down whatever the active modes set up, in reverse order. Safe to call
 * when nothing was set (modes 0 and 1). */
static void passthrough_cleanup(void) {
    int touched = probe_registered || hooks_registered || gate_opened_by_us;
    if (window_task) { kthread_stop(window_task); window_task = (void *)0; }
    if (probe_registered) { unregister_kprobe(&route_probe); probe_registered = 0; }
    gate_restore();
    hooks_unregister();
    if (touched) synchronize_rcu();
}

static int passthrough_init(void) {
    const char *reason;
    passthrough_derive();
    reason = earc_mode_refusal(&hdmi, mode);
    if (reason) {
        printk("\0013earc_route: refused: %s\n", reason);
        return -22;
    }
    /* The modes call register_kprobe directly; confirm the kernel resolved the
     * same address the loader passed as fn_register_kprobe (a bad kallsyms). */
    if (hdmi.kprobe_register && hdmi.kprobe_register != (ed_u64)(unsigned long)&register_kprobe) {
        printk("\0013earc_route: refused; register_kprobe %llx != resolved %lx\n",
               hdmi.kprobe_register, (unsigned long)&register_kprobe);
        return -22;
    }
    switch (mode) {
    case 2: return selftest_mode();
    case 3: return gate_mode();
    case 4: return session_mode(0);
    case 5: return session_mode(1);
    default: return -22;
    }
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
    if (carrier_rate == 192000 && (output_clock(&original_clock_rate, &original_clock_flag, &original_clock_channels) ||
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
        hdmi_audit();
        printk("\0016earc_route: audit complete; unloading\n");
        return -125;
    }
    if (mode == 1) return apply();
    return passthrough_init();
}

void cleanup_module(void) {
    passthrough_cleanup();
    if (watchdog) kthread_stop(watchdog);
    restore_route();
    printk("\0016earc_route: unloaded\n");
}

/* Linux's loader initialises the remaining fields. The size and the init and
 * exit offsets are those of the stock aspectratiodrv.ko of the generation
 * (5.4.268: 0x380/0x150/0x338; 6.12.44: 0x4c0/0x138/0x480; name at 24 in both);
 * build.py and the loader check them. */
struct target_module {
    u8 prefix[24];
    char name[56];
    u8 before_init[EARC_MODULE_INIT - 80];
    int (*initialise)(void);
    u8 before_exit[EARC_MODULE_EXIT - EARC_MODULE_INIT - 8];
    void (*finish)(void);
    u8 suffix[EARC_MODULE_SIZE - EARC_MODULE_EXIT - 8];
};
_Static_assert(sizeof(struct target_module) == EARC_MODULE_SIZE, "module size");
_Static_assert(__builtin_offsetof(struct target_module, name) == 24, "module name");
_Static_assert(__builtin_offsetof(struct target_module, initialise) == EARC_MODULE_INIT, "module init");
_Static_assert(__builtin_offsetof(struct target_module, finish) == EARC_MODULE_EXIT, "module exit");
struct target_module __this_module __attribute__((section(".gnu.linkonce.this_module"), aligned(64), used)) = {
    .name = "earc_route", .initialise = init_module, .finish = cleanup_module
};

/* Linux 5.4 and 6.12 moduleparam.h: struct kernel_param_ops is {flags, set, get,
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
#define EARC_HDMI_SIZE_PARAMETER_1(name) {"sz_" #name, &__this_module, &uint_operations, 0, -1, 0, &sz_##name},
#define EARC_HDMI_SIZE_PARAMETER_0(name)
#define EARC_HDMI_PARAMETERS(name, analysed) \
    {"fn_" #name, &__this_module, &ulong_operations, 0, -1, 0, &fn_##name}, \
    EARC_HDMI_SIZE_PARAMETER_##analysed(name)
static const struct target_parameter parameters[] __attribute__((section("__param"), aligned(8), used)) = {
    EARC_FUNCTIONS(EARC_PARAMETERS)
    EARC_HDMI_FUNCTIONS(EARC_HDMI_PARAMETERS)
    {"fn__stext", &__this_module, &ulong_operations, 0, -1, 0, &fn__stext},
    {"fn__etext", &__this_module, &ulong_operations, 0, -1, 0, &fn__etext},
    {"mode", &__this_module, &uint_operations, 0400, -1, 0, &mode},
    {"renderer", &__this_module, &uint_operations, 0400, -1, 0, &renderer},
    {"lease", &__this_module, &uint_operations, 0600, -1, 0, &lease},
    {"active", &__this_module, &uint_operations, 0400, -1, 0, &applied},
    {"restore_pcm", &__this_module, &uint_operations, 0400, -1, 0, &restore_pcm},
    {"carrier_rate", &__this_module, &uint_operations, 0400, -1, 0, &carrier_rate},
    {"hd_ready", &__this_module, &uint_operations, 0400, -1, 0, &hd_ready},
    /* HDMI passthrough (mode>=2): load-time inputs and read-only state. */
    {"window_seconds", &__this_module, &uint_operations, 0400, -1, 0, &window_seconds},
    {"supervised", &__this_module, &uint_operations, 0400, -1, 0, &supervised},
    {"gate_open", &__this_module, &uint_operations, 0400, -1, 0, &gate_open},
    {"gate_opened_by_us", &__this_module, &uint_operations, 0400, -1, 0, &gate_opened_by_us},
    {"hooks_registered", &__this_module, &uint_operations, 0400, -1, 0, &hooks_registered},
    {"probe_registered", &__this_module, &uint_operations, 0400, -1, 0, &probe_registered},
    {"selftest_passed", &__this_module, &uint_operations, 0400, -1, 0, &selftest_passed},
    {"window_open", &__this_module, &uint_operations, 0400, -1, 0, &window_open},
    {"calls_seen", &__this_module, &uint_operations, 0400, -1, 0, &calls_seen},
    {"calls_matched", &__this_module, &uint_operations, 0400, -1, 0, &calls_matched},
    {"overrides", &__this_module, &uint_operations, 0400, -1, 0, &overrides}
};
static const char module_license[] __attribute__((section(".modinfo"), used)) = "license=GPL";
static const char module_vermagic[] __attribute__((section(".modinfo"), used)) = "vermagic=" EARC_VERMAGIC;
static const char module_description[] __attribute__((section(".modinfo"), used)) =
    "description=Self-adapting port of Glasshouse's native DTS eARC forwarding route";
__asm__(".pushsection .plt,\"ax\",%progbits\n.byte 0\n.popsection\n"
        ".pushsection .init.plt,\"a\",%progbits\n.byte 0\n.popsection\n"
        ".pushsection .text.ftrace_trampoline,\"ax\",%progbits\n.byte 0\n.popsection\n");
