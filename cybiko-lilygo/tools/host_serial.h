#ifndef CYBIKO_HOST_SERIAL_H
#define CYBIKO_HOST_SERIAL_H

#include <stddef.h>

#define HOST_SERIAL_OTHER  0
#define HOST_SERIAL_HELTEC 1  /* Silicon Labs CP2102, Heltec V3 */
#define HOST_SERIAL_TDECK  2  /* Espressif USB JTAG/serial */

#define HOST_SERIAL_MAX 16

typedef struct {
    char port[64];
    char label[96];
    unsigned vid, pid;
    int kind;
} host_serial_dev_t;

int host_list_serial(host_serial_dev_t *out, int cap);
int host_find_kind(int kind, const char *prefer,
                   char *port, size_t pcap, char *label, size_t lcap);
void host_exe_dir(char *out, size_t cap);

#endif
