#ifndef VX_SYSINFO_H
#define VX_SYSINFO_H

struct vx_sys {
    int battery;        /* percent, -1 unknown */
    int charging;
    int wifi_up;
    long mem_total_kb, mem_avail_kb;
    long self_rss_kb;
    long uptime_s;
    char kernel[48];
};

void sys_update(struct vx_sys *s);
void sys_update_mem(struct vx_sys *s);

/* Backlight in percent; dir "auto" picks the first /sys/class/backlight. */
int  sys_backlight_init(const char *dir);
int  sys_backlight_get(void);
void sys_backlight_set(int percent);

#endif
