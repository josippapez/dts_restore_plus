/* hdroute-uimc: connect or disconnect stock audio DSP module ports through
 * LG's /dev/lg/audio_uimc, for hdroute.sh. ioctl ABI from LG's GPL
 * kdriver/include/audio_uimc.h (AUIMC_IOC_MAGIC 'M').
 *   hdroute-uimc connect OUT_UID OUT_PORT IN_UID IN_PORT
 *   hdroute-uimc disconnect-input UID PORT */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct connection {
    uint32_t out_uid;
    int32_t out_port;
    uint32_t in_uid;
    int32_t in_port;
};

struct disconnection {
    uint32_t uid;
    int32_t port;
};

_Static_assert(sizeof(struct connection) == 16, "connection ABI");
_Static_assert(sizeof(struct disconnection) == 8, "disconnection ABI");

static uint32_t number(const char *text, uint32_t max)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno || !*text || *text == '-' || *end || value > max) {
        fprintf(stderr, "invalid number: %s\n", text);
        exit(2);
    }
    return (uint32_t)value;
}

int main(int argc, char **argv)
{
    struct connection edge;
    struct disconnection input;
    unsigned long request;
    void *argument;
    int fd, result;

    if (argc == 6 && !strcmp(argv[1], "connect")) {
        edge.out_uid = number(argv[2], UINT32_MAX);
        edge.out_port = (int32_t)number(argv[3], INT32_MAX);
        edge.in_uid = number(argv[4], UINT32_MAX);
        edge.in_port = (int32_t)number(argv[5], INT32_MAX);
        request = _IOW('M', 8, struct connection);
        argument = &edge;
    } else if (argc == 4 && !strcmp(argv[1], "disconnect-input")) {
        input.uid = number(argv[2], UINT32_MAX);
        input.port = (int32_t)number(argv[3], INT32_MAX);
        request = _IOW('M', 15, struct disconnection);
        argument = &input;
    } else {
        fprintf(stderr, "usage: %s connect OUT_UID OUT_PORT IN_UID IN_PORT | disconnect-input UID PORT\n",
                argv[0]);
        return 2;
    }
    fd = open("/dev/lg/audio_uimc", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/lg/audio_uimc");
        return 1;
    }
    result = ioctl(fd, request, argument);
    if (result < 0)
        perror(argv[1]);
    close(fd);
    return result < 0 ? 1 : 0;
}
