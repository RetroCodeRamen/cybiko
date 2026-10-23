#include "host_serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <io.h>
#else
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#endif

static int kind_from_ids(unsigned vid, unsigned pid) {
    if (vid == 0x10c4u && pid == 0xea60u) return HOST_SERIAL_HELTEC;
    if (vid == 0x10c4u) return HOST_SERIAL_HELTEC;
    if (vid == 0x303au && (pid == 0x1001u || pid == 0u)) return HOST_SERIAL_TDECK;
    if (vid == 0x303au) return HOST_SERIAL_TDECK;
    return HOST_SERIAL_OTHER;
}

static void classify(host_serial_dev_t *d) {
    d->kind = kind_from_ids(d->vid, d->pid);
    if (d->kind == HOST_SERIAL_HELTEC && !d->label[0])
        snprintf(d->label, sizeof d->label, "Heltec CP2102");
    if (d->kind == HOST_SERIAL_TDECK && !d->label[0])
        snprintf(d->label, sizeof d->label, "T-Deck ESP32-S3");
}

#ifdef _WIN32

void host_exe_dir(char *out, size_t cap) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    if (!n || n >= MAX_PATH) {
        out[0] = 0;
        return;
    }
    for (slash = path; *slash; slash++)
        if (*slash == '\\') *slash = '/';
    slash = strrchr(path, '/');
    if (slash) *slash = 0;
    snprintf(out, cap, "%s", path);
}

static int parse_vid_pid(const char *hw, unsigned *vid, unsigned *pid) {
    const char *v, *p;
    if (!hw) return 0;
    v = strstr(hw, "VID_");
    p = strstr(hw, "PID_");
    if (!v) v = strstr(hw, "Vid_");
    if (!p) p = strstr(hw, "Pid_");
    if (!v || !p) return 0;
    *vid = (unsigned)strtoul(v + 4, NULL, 16);
    *pid = (unsigned)strtoul(p + 4, NULL, 16);
    return 1;
}

int host_list_serial(host_serial_dev_t *out, int cap) {
    HDEVINFO info;
    SP_DEVINFO_DATA dev;
    int n = 0, i;
    if (!out || cap <= 0) return 0;
    info = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return 0;
    dev.cbSize = sizeof dev;
    for (i = 0; SetupDiEnumDeviceInfo(info, (DWORD)i, &dev) && n < cap; i++) {
        char hw[256] = "", friendly[256] = "", port[64] = "";
        HKEY key;
        DWORD type, sz;
        host_serial_dev_t *d = &out[n];
        memset(d, 0, sizeof *d);
        sz = sizeof hw;
        SetupDiGetDeviceRegistryPropertyA(info, &dev, SPDRP_HARDWAREID,
                                          &type, (BYTE *)hw, sz, &sz);
        sz = sizeof friendly;
        SetupDiGetDeviceRegistryPropertyA(info, &dev, SPDRP_FRIENDLYNAME,
                                          &type, (BYTE *)friendly, sz, &sz);
        key = SetupDiOpenDevRegKey(info, &dev, DICS_FLAG_GLOBAL, 0,
                                   DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            sz = sizeof port;
            if (RegQueryValueExA(key, "PortName", NULL, &type,
                                 (BYTE *)port, &sz) != ERROR_SUCCESS)
                port[0] = 0;
            RegCloseKey(key);
        }
        if (!port[0]) continue;
        snprintf(d->port, sizeof d->port, "%s", port);
        if (friendly[0])
            snprintf(d->label, sizeof d->label, "%s", friendly);
        parse_vid_pid(hw, &d->vid, &d->pid);
        classify(d);
        n++;
    }
    SetupDiDestroyDeviceInfoList(info);
    return n;
}

#else

void host_exe_dir(char *out, size_t cap) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    if (n <= 0) {
        out[0] = 0;
        return;
    }
    exe[n] = 0;
    slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(out, cap, "%s", exe);
}

static int usb_ids_label(const char *dev, unsigned *vid, unsigned *pid,
                         char *label, size_t lcap) {
    const char *base = strrchr(dev, '/');
    char start[256], cur[PATH_MAX];
    base = base ? base + 1 : dev;
    snprintf(start, sizeof start, "/sys/class/tty/%s/device", base);
    if (!realpath(start, cur))
        snprintf(cur, sizeof cur, "%s", start);
    for (int i = 0; i < 8; i++) {
        char fpath[PATH_MAX + 24];
        FILE *f;
        snprintf(fpath, sizeof fpath, "%s/idVendor", cur);
        f = fopen(fpath, "r");
        if (f) {
            int ok = fscanf(f, "%x", vid) == 1;
            fclose(f);
            if (!ok) return 0;
            snprintf(fpath, sizeof fpath, "%s/idProduct", cur);
            f = fopen(fpath, "r");
            if (!f) return 0;
            ok = fscanf(f, "%x", pid) == 1;
            fclose(f);
            if (!ok) return 0;
            if (label && lcap) {
                char prod[80] = "";
                snprintf(fpath, sizeof fpath, "%s/product", cur);
                f = fopen(fpath, "r");
                if (f) {
                    if (fgets(prod, sizeof prod, f)) {
                        size_t n = strlen(prod);
                        while (n && (prod[n - 1] == '\n' || prod[n - 1] == '\r'))
                            prod[--n] = 0;
                    }
                    fclose(f);
                }
                if (prod[0])
                    snprintf(label, lcap, "%s", prod);
            }
            return 1;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;
        *slash = 0;
    }
    return 0;
}

int host_list_serial(host_serial_dev_t *out, int cap) {
    DIR *d;
    struct dirent *ent;
    int n = 0;
    if (!out || cap <= 0) return 0;
    d = opendir("/dev");
    if (!d) return 0;
    while ((ent = readdir(d)) != NULL && n < cap) {
        int usb = strncmp(ent->d_name, "ttyUSB", 6) == 0;
        int acm = strncmp(ent->d_name, "ttyACM", 6) == 0;
        host_serial_dev_t *dev;
        if (!usb && !acm) continue;
        if (strlen(ent->d_name) > 48) continue;
        dev = &out[n];
        memset(dev, 0, sizeof *dev);
        snprintf(dev->port, sizeof dev->port, "/dev/%s", ent->d_name);
        if (usb_ids_label(dev->port, &dev->vid, &dev->pid,
                          dev->label, sizeof dev->label)) {
            classify(dev);
            n++;
        }
    }
    closedir(d);
    return n;
}

#endif

int host_find_kind(int kind, const char *prefer,
                   char *port, size_t pcap, char *label, size_t lcap) {
    host_serial_dev_t list[HOST_SERIAL_MAX];
    int n = host_list_serial(list, HOST_SERIAL_MAX);
    int i, exact = -1, any = -1, pref = -1;
    for (i = 0; i < n; i++) {
        if (list[i].kind != kind) continue;
        if (any < 0) any = i;
        if (kind == HOST_SERIAL_HELTEC && list[i].vid == 0x10c4u
            && list[i].pid == 0xea60u && exact < 0)
            exact = i;
        if (kind == HOST_SERIAL_TDECK && list[i].vid == 0x303au
            && list[i].pid == 0x1001u && exact < 0)
            exact = i;
        if (prefer && prefer[0] && strcmp(list[i].port, prefer) == 0)
            pref = i;
    }
    i = pref >= 0 ? pref : (exact >= 0 ? exact : any);
    if (i < 0) return 0;
    if (port && pcap)
        snprintf(port, pcap, "%s", list[i].port);
    if (label && lcap) {
        if (list[i].label[0])
            snprintf(label, lcap, "%s", list[i].label);
        else
            snprintf(label, lcap, "%s", list[i].port);
    }
    return 1;
}
