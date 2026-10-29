#include "sysinfo.h"
#include "vx.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static char bl_dir[256];
static long bl_max;

static void battery(struct vx_sys *s)
{
    DIR *d = opendir("/sys/class/power_supply");
    struct dirent *e;
    s->battery = -1;
    s->charging = 0;
    if (!d)
        return;
    while ((e = readdir(d))) {
        char path[320], type[16] = "";
        FILE *f;
        long cap;
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
        if ((f = fopen(path, "r"))) {
            if (!fgets(type, sizeof(type), f))
                type[0] = '\0';
            fclose(f);
        }
        if (strncmp(type, "Battery", 7) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        if (vx_read_int(path, &cap) == 0)
            s->battery = (int)cap;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(path, "r"))) {
            char st[24] = "";
            if (fgets(st, sizeof(st), f))
                s->charging = strncmp(st, "Charging", 8) == 0 || strncmp(st, "Full", 4) == 0;
            fclose(f);
        }
        break;
    }
    closedir(d);
}

void sys_update_mem(struct vx_sys *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long pages;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "MemTotal: %ld kB", &s->mem_total_kb);
            sscanf(line, "MemAvailable: %ld kB", &s->mem_avail_kb);
        }
        fclose(f);
    }
    f = fopen("/proc/self/statm", "r");
    if (f) {
        long size;
        if (fscanf(f, "%ld %ld", &size, &pages) == 2)
            s->self_rss_kb = pages * (sysconf(_SC_PAGESIZE) / 1024);
        fclose(f);
    }
}

void sys_update(struct vx_sys *s)
{
    struct sysinfo si;
    struct utsname u;
    long carrier;
    battery(s);
    s->wifi_hw = access("/sys/class/net/wlan0", F_OK) == 0;
    s->wifi_on = 0;
    if (s->wifi_hw) {
        /* IFF_UP; "flags" is hex, e.g. 0x1003 */
        FILE *f = fopen("/sys/class/net/wlan0/flags", "r");
        unsigned long flags = 0;
        if (f) {
            if (fscanf(f, "%lx", &flags) != 1)
                flags = 0;
            fclose(f);
        }
        s->wifi_on = flags & 1;
    }
    s->wifi_up = vx_read_int("/sys/class/net/wlan0/carrier", &carrier) == 0 && carrier == 1;
    s->net_up = s->wifi_up;
    if (!s->net_up) {
        DIR *d = opendir("/sys/class/net");
        struct dirent *e;
        while (d && (e = readdir(d)) && !s->net_up) {
            char path[300];
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
                continue;
            snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", e->d_name);
            s->net_up = vx_read_int(path, &carrier) == 0 && carrier == 1;
        }
        if (d)
            closedir(d);
    }
    sys_update_mem(s);
    if (sysinfo(&si) == 0)
        s->uptime_s = si.uptime;
    if (!s->kernel[0] && uname(&u) == 0)
        snprintf(s->kernel, sizeof(s->kernel), "%s", u.release);
}

int sys_backlight_init(const char *dir)
{
    char path[300];
    if (strcmp(dir, "auto") == 0) {
        DIR *d = opendir("/sys/class/backlight");
        struct dirent *e;
        bl_dir[0] = '\0';
        if (!d)
            return -1;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.') {
                snprintf(bl_dir, sizeof(bl_dir), "/sys/class/backlight/%s", e->d_name);
                break;
            }
        closedir(d);
    } else {
        vx_strlcpy(bl_dir, dir, sizeof(bl_dir));
    }
    if (!bl_dir[0])
        return -1;
    snprintf(path, sizeof(path), "%s/max_brightness", bl_dir);
    if (vx_read_int(path, &bl_max) != 0 || bl_max <= 0) {
        bl_dir[0] = '\0';
        return -1;
    }
    return 0;
}

int sys_backlight_get(void)
{
    char path[300];
    long v;
    if (!bl_dir[0])
        return -1;
    snprintf(path, sizeof(path), "%s/brightness", bl_dir);
    if (vx_read_int(path, &v) != 0)
        return -1;
    return (int)((v * 100 + bl_max / 2) / bl_max);
}

void sys_backlight_set(int percent)
{
    char path[300], val[24];
    if (!bl_dir[0])
        return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    snprintf(path, sizeof(path), "%s/brightness", bl_dir);
    snprintf(val, sizeof(val), "%ld", (percent * bl_max + 50) / 100);
    vx_write_str(path, val);
}
