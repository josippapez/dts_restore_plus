/* SPDX-License-Identifier: GPL-2.0-only */
/* /dev/earc_route lease, shared by every target. The latest writer of "1" or
 * "2" (renderer index + 1) holds it; the kernel's release of that file (close,
 * crash or kill) drops it, and a stale holder's release leaves a newer lease alone.
 * Before including: EARC_LEASE_COPY_BYTE(to, from) returning nonzero on fault,
 * and optionally EARC_LEASE_RELEASED(previous) for a target's own log. */
#ifndef EARC_CORE_LEASE_H
#define EARC_CORE_LEASE_H

/* A target that reads the lease before this point declares the state itself. */
#ifndef EARC_LEASE_STATE_DEFINED
static unsigned long lease_word;
static unsigned int app_route;
#define APP_ROUTE() ((unsigned int)(__atomic_load_n(&lease_word, __ATOMIC_ACQUIRE) & 3))
#endif

struct target_file;
static int lease_open(void *inode, struct target_file *file) {
    (void)inode; (void)file;
    return 0;
}
static int lease_release(void *inode, struct target_file *file) {
    unsigned long word = __atomic_load_n(&lease_word, __ATOMIC_ACQUIRE);
    (void)inode;
    if ((word & ~3UL) == (unsigned long)file &&
        __atomic_compare_exchange_n(&lease_word, &word, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&app_route, 0, __ATOMIC_RELEASE);
#ifdef EARC_LEASE_RELEASED
        EARC_LEASE_RELEASED(word);
#endif
    }
    return 0;
}
static long lease_write(struct target_file *file, const char *user, unsigned long length, long long *position) {
    char value = 0;
    (void)position;
    if (length < 1 || length > 2 || EARC_LEASE_COPY_BYTE(&value, user)) return -22;
    if (value < '0' || value > '2') return -22;
    __atomic_store_n(&lease_word, (unsigned long)file | (unsigned long)(value - '0'), __ATOMIC_RELEASE);
    __atomic_store_n(&app_route, (unsigned int)(value - '0'), __ATOMIC_RELEASE);
    return (long)length;
}

/* struct miscdevice is minor, name, fops (+0, +8, +16) on 4.4, 5.4 and 6.12. */
struct target_miscdevice { int minor; unsigned char pad[4]; const char *name; void *fops; unsigned char tail[0x60]; };
static void *lease_fops[0x100 / 8];
static struct target_miscdevice lease_device;

/* Fills lease_fops at the kernel's write/open/release slots and names the device. */
#define lease_prepare(owner, write, open, release)                \
    do {                                                          \
        lease_fops[0] = (owner);                                  \
        lease_fops[(write) / 8] = (void *)lease_write;            \
        lease_fops[(open) / 8] = (void *)lease_open;              \
        lease_fops[(release) / 8] = (void *)lease_release;        \
        lease_device.minor = 255; /* MISC_DYNAMIC_MINOR */        \
        lease_device.name = "earc_route";                         \
        lease_device.fops = lease_fops;                           \
    } while (0)

#endif
