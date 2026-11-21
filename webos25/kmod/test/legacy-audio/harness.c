typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

extern const u8 real_module[], real_module_end[];
extern u8 harness_bss_start[], harness_bss_end[];

/* Kernel symbols: run.sh defines each from the capture's kallsyms at link time. */
extern int printk(const char *, ...);
extern long sys_init_module(const void *, u64, const char *);
extern long sys_delete_module(const char *, unsigned int);
extern long sys_open(const char *, int, int);
extern long sys_read(unsigned int, char *, u64);
extern long sys_write(unsigned int, const char *, u64);
extern long sys_close(unsigned int);
extern long sys_mkdir(const char *, int);
extern long sys_mount(const char *, const char *, const char *, u64, const void *);
extern void flush_delayed_fput(void);
extern int aarch64_insn_patch_text(void **, u32 *, int);
extern void get_online_cpus(void), put_online_cpus(void);
extern void *kthread_create_on_node(int (*)(void *), void *, int, const char *, ...);
extern int wake_up_process(void *), kthread_stop(void *);
extern _Bool kthread_should_stop(void);
extern void msleep(unsigned int);
extern int AUDIO_SetIecOutEncodedType(u32, u32);
extern int sndout_earc_output_type(void *, void *);
extern void _AUDIO_SetSRCFormat(void *, u32, u32, u32, u32, u32, u32);
extern u32 g_AudArcIecOutEncodedType[], g_AudSpdifIecOutEncodedType[];
extern volatile u8 _gSndOutEarcOnOff;
extern volatile u32 _gMainAudioIndex, gEarcOutputType[], __logm_fd_alsa_sndout, __logm_fd_aud_master;

#define ARC_TYPE (*(volatile u32 *)g_AudArcIecOutEncodedType)

static u32 captured_src[9];
static int failures;

void *memset(void *target, int value, unsigned long length)
{
    volatile u8 *cursor = target;
    while (length--)
        *cursor++ = (u8)value;
    return target;
}

int harness_imc(u32 command, u32 module, u32 size, const u32 *data)
{
    u32 index;
    (void)module;
    if (command == 0x50020020 && size == 32) {
        for (index = 0; index < 8; index++)
            captured_src[index] = data[index];
        captured_src[8]++;
    }
    return 0;
}

int harness_noop(void)
{
    return 0;
}

static void check(int condition, const char *what)
{
    if (!condition) {
        failures++;
        printk("\0013LEGACY_AUDIO: FAIL %s\n", what);
    }
}

static u32 param(const char *name)
{
    char path[96] = "/sys/module/earc_route/parameters/", buffer[16] = {0};
    u32 index = 0, value = 0;
    long fd, length;
    while (path[index])
        index++;
    while (*name)
        path[index++] = *name++;
    fd = sys_open(path, 0, 0);
    if (fd < 0)
        return 0xffffffffU;
    length = sys_read(fd, buffer, sizeof buffer - 1);
    sys_close(fd);
    for (index = 0; index < (u32)(length > 0 ? length : 0) && buffer[index] >= '0' && buffer[index] <= '9'; index++)
        value = value * 10 + (u32)(buffer[index] - '0');
    return value;
}

static long set_enabled(const char *value)
{
    long fd = sys_open("/sys/module/earc_route/parameters/enabled", 1, 0), result;
    if (fd < 0)
        return fd;
    result = sys_write(fd, value, 1);
    sys_close(fd);
    return result;
}

static u32 iec(u32 requested)
{
    AUDIO_SetIecOutEncodedType(3, requested);
    return ARC_TYPE;
}

/* Presets LG's last-applied settings so an unchanged result skips the hardware reconnect. */
static int mix(u32 expected)
{
    volatile u32 *applied = gEarcOutputType + 1;
    u64 request[24] = {0};
    applied[0] = 2; applied[1] = 6; applied[2] = 8; applied[3] = 192000; applied[4] = expected;
    request[9] = 2; request[10] = 6; request[11] = 8; request[12] = 192000; request[13] = 1;
    return sndout_earc_output_type((void *)0, request) ? -1 : (int)request[13];
}

static u32 src(u32 device_index, u32 rate, u32 channels, u32 bits)
{
    u8 renderer[512] = {0}, device[256] = {0};
    *(u8 **)(renderer + 0x1b0) = device;
    *(u32 *)(renderer + 0x1a8) = 1;
    *(u32 *)(device + 0x8c) = device_index;
    _AUDIO_SetSRCFormat(renderer, 0x789, rate, channels, bits, 0, 0);
    return captured_src[5] == 192000 && captured_src[6] == 8 && captured_src[7] == 16;
}

static u32 stress_calls, stress_errors;

/* Hammers the three hooked LG functions while the lease toggles, from several CPUs. */
static int stress_worker(void *argument)
{
    u64 request[24];
    u32 index, kind = (u32)(u64)argument;
    while (!kthread_should_stop()) {
        for (index = 0; index < 64; index++) {
            if (kind == 0) {
                AUDIO_SetIecOutEncodedType(3, 0);
                if (ARC_TYPE != 0 && ARC_TYPE != 6)
                    __atomic_add_fetch(&stress_errors, 1, __ATOMIC_RELAXED);
            } else if (kind == 1) {
                memset(request, 0, sizeof request);
                request[9] = 9;
                if (sndout_earc_output_type((void *)0, request) != -1)
                    __atomic_add_fetch(&stress_errors, 1, __ATOMIC_RELAXED);
            } else {
                u8 renderer[512], device[256];
                memset(renderer, 0, sizeof renderer);
                memset(device, 0, sizeof device);
                *(u8 **)(renderer + 0x1b0) = device;
                _AUDIO_SetSRCFormat(renderer, 0x789, 48000, 2, 16, 0, 0);
            }
            __atomic_add_fetch(&stress_calls, 1, __ATOMIC_RELAXED);
        }
        msleep(1);
    }
    return 0;
}

static void stress_test(long holder)
{
    void *workers[3];
    u32 index, round, started = 0, hits_before = param("hits");
    for (index = 0; index < 3; index++) {
        workers[index] = kthread_create_on_node(stress_worker, (void *)(u64)index, -1, "earc-stress-%u", index);
        if ((u64)workers[index] >= ~4094UL)
            break;
        wake_up_process(workers[index]);
        started++;
    }
    for (round = 0; round < 40; round++) {
        sys_write(holder, round & 1 ? "0" : "1", 1);
        msleep(5);
    }
    for (index = 0; index < started; index++)
        kthread_stop(workers[index]);
    sys_write(holder, "0", 1);
    printk("\0016LEGACY_AUDIO: stress workers=%u calls=%u errors=%u hits=%u\n", started, stress_calls,
           stress_errors, param("hits") - hits_before);
    check(started == 3 && stress_calls > 3000 && !stress_errors && param("hits") - hits_before >= stress_calls,
          "concurrent hooked calls across lease toggles");
}

static void policy_tests(void)
{
    long first, second;
    u32 sites[3];
    first = sys_open("/dev/earc_route", 2, 0);
    second = sys_open("/dev/earc_route", 2, 0);
    check(first >= 0 && second >= 0, "real /dev/earc_route opens");
    _gSndOutEarcOnOff = 1;
    _gMainAudioIndex = 0;
    check(iec(0) == 0 && mix(1) == 1 && !src(0, 192000, 8, 16), "no lease: stock PCM, mixing, 48k/32 converter");

    check(sys_write(first, "1", 1) == 1 && param("lease") == 1, "lease 1 taken");
    check(iec(0) == 6, "lease on main 0: eARC encoded type forced to HBR(6)");
    check(iec(1) == 6, "lease on main 0: DD request also forced to HBR");
    check(mix(0) == 0, "lease: LG eARC mix flag cleared");
    check(src(0, 192000, 8, 16), "lease: converter kept at 192k/8ch/16bit");
    check(!src(1, 192000, 8, 16), "lease: other renderer converter untouched");
    check(!src(0, 48000, 2, 16), "lease: stereo 48k converter untouched");
    AUDIO_SetIecOutEncodedType(2, 1);
    check(*(volatile u32 *)g_AudSpdifIecOutEncodedType == 1 && ARC_TYPE == 6, "optical port untouched");
    _gMainAudioIndex = 1;
    check(iec(0) == 0 && mix(1) == 1, "lease for renderer 0 ignored while main audio is 1");
    _gMainAudioIndex = 0;
    _gSndOutEarcOnOff = 0;
    check(iec(0) == 0, "eARC off: stock type kept");
    _gSndOutEarcOnOff = 1;
    check(sys_write(first, "9", 1) == -22 && sys_write(first, "", 0) == -22
          && sys_write(first, "111", 3) == -22 && param("lease") == 1, "invalid lease writes rejected");

    check(set_enabled("0") == 1 && iec(0) == 0 && mix(1) == 1 && !src(0, 192000, 8, 16), "enabled=0: full stock behaviour");
    check(set_enabled("1") == 1 && iec(0) == 6, "enabled=1: policy back");

    check(sys_write(second, "2", 1) == 1 && param("lease") == 2, "second holder takes lease 2");
    _gMainAudioIndex = 1;
    check(iec(0) == 6 && src(1, 192000, 8, 16), "lease 2 routes renderer 1");
    sys_close(first);
    flush_delayed_fput();
    check(param("lease") == 2 && iec(0) == 6, "stale holder close keeps new lease");
    sys_close(second);
    flush_delayed_fput();
    check(param("lease") == 0 && iec(0) == 0 && mix(1) == 1, "owner close releases lease and restores stock");
    _gMainAudioIndex = 0;
    first = sys_open("/dev/earc_route", 2, 0);
    stress_test(first);
    sys_close(first);
    flush_delayed_fput();

    check(sys_delete_module("earc_route", 0) == -16, "rmmod refused: module is permanent (no exit)");
    check(sys_init_module(real_module, real_module_end - real_module, "") == -17, "second load refused");
    sites[0] = *(volatile u32 *)((u32 *)AUDIO_SetIecOutEncodedType);
    sites[1] = *(volatile u32 *)((u32 *)sndout_earc_output_type);
    sites[2] = *(volatile u32 *)(((u32 *)_AUDIO_SetSRCFormat) + 0x6c / 4);
    check(sites[0] == 0xd4298620 && sites[1] == 0xd4298620 && sites[2] == 0xd4298620, "three BRK sites armed");
    printk("\0016LEGACY_AUDIO: counters hits=%u overrides=%u mix_clears=%u src_fixes=%u\n",
           param("hits"), param("overrides"), param("mix_clears"), param("src_fixes"));
    check(param("overrides") >= 4 && param("mix_clears") >= 1 && param("src_fixes") >= 2, "counters advanced");
}

static int refusal_test(void)
{
    void *address = ((u32 *)sndout_earc_output_type) + 4;
    u32 original = *(volatile u32 *)address, changed = 0xd503201f;
    long result;
    get_online_cpus();
    aarch64_insn_patch_text(&address, &changed, 1);
    put_online_cpus();
    result = sys_init_module(real_module, real_module_end - real_module, "");
    get_online_cpus();
    aarch64_insn_patch_text(&address, &original, 1);
    put_online_cpus();
    printk("\0016LEGACY_AUDIO: refusal on modified LG code result=%ld expected=-19\n", result);
    return result == -19 && *(volatile u32 *)address == original;
}

__attribute__((section(".text.entry"))) int harness_entry(void)
{
    u64 stack, saved;
    volatile u64 *limit;
    long result;
    for (volatile u8 *cursor = harness_bss_start; cursor < harness_bss_end; cursor++)
        *cursor = 0;
    __asm__ volatile("mov %0, sp" : "=r"(stack));
    limit = (volatile u64 *)((stack & ~16383UL) + 8);
    saved = *limit;
    *limit = ~0UL;
    __logm_fd_alsa_sndout = 0;
    __logm_fd_aud_master = 0;
    check(iec(1) == 1, "before load: stock DD type kept");
    sys_mkdir("/dev", 0755);
    check(sys_mount("devtmpfs", "/dev", "devtmpfs", 0, 0) == 0, "mount devtmpfs");
    sys_mkdir("/sys", 0755);
    check(sys_mount("sysfs", "/sys", "sysfs", 0, 0) == 0, "mount sysfs");
    printk("\0016LEGACY_AUDIO: loading the real earc_route-4.4.ko through sys_init_module\n");
    check(refusal_test(), "fingerprint refusal");
    result = sys_init_module(real_module, real_module_end - real_module, "");
    printk("\0016LEGACY_AUDIO: load result=%ld\n", result);
    check(result == 0, "real module loads");
    if (!result)
        policy_tests();
    *limit = saved;
    printk("\0016LEGACY_AUDIO: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    __asm__ volatile("movz x0, #0x8\nmovk x0, #0x8400, lsl #16\nhvc #0" ::: "x0", "memory");
    for (;;)
        __asm__ volatile("wfe");
}
