/*
 * rtcdev.c - /dev/rtc0, the real-time clock as Linux's RTC device
 * (shell completeness S5: BusyBox `hwclock`).
 *
 * Two of Linux's requests through the character device's ioctl hook,
 * which only the Linux personality calls: RTC_RD_TIME reads the clock
 * (arch_rtc_read_epoch) and RTC_SET_TIME sets it (arch_rtc_write_epoch),
 * both as a struct rtc_time in UTC. Setting needs PRIV_SYS_ADMIN, the
 * privilege that sets the system clock, and is -EACCES without it, as
 * Linux's CAP_SYS_TIME check is. The node exists where there is an RTC
 * (arch_rtc_present), whatever date it holds, so an RTC reading an invalid
 * date can still be set (found in review): QEMU's q35 and virt; not
 * Parallels Desktop's AArch64 machine, whose wall clock comes from the
 * firmware.
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>

#include <arch/timer.h>

/* Linux's struct rtc_time and its two requests: _IOR('p', 0x09, ...) and
 * _IOW('p', 0x0a, ...), 36 bytes. */
struct lx_rtc_time {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
#define LX_RTC_RD_TIME  0x80247009u
#define LX_RTC_SET_TIME 0x4024700au

static bool leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static unsigned days_in_month(int64_t y, unsigned m)
{
    static const unsigned d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

static int rtc_get(struct lx_rtc_time *tm)
{
    uint64_t s;
    if (!arch_rtc_read_epoch(&s))
        return -EINVAL;   /* not a valid date, as Linux's rtc_read_time says */
    int64_t days = (int64_t)(s / 86400), y;
    unsigned m, d, rem = (unsigned)(s % 86400);
    time_civil_from_days(days, &y, &m, &d);
    *tm = (struct lx_rtc_time){
        .tm_sec = (int32_t)(rem % 60),
        .tm_min = (int32_t)(rem / 60 % 60),
        .tm_hour = (int32_t)(rem / 3600),
        .tm_mday = (int32_t)d,
        .tm_mon = (int32_t)m - 1,
        .tm_year = (int32_t)(y - 1900),
        .tm_wday = (int32_t)((days + 4) % 7),   /* 1970-01-01 was a Thursday */
        .tm_yday = (int32_t)(days - time_days_from_civil(y, 1, 1)),
    };
    return 0;
}

/* Linux's rtc_valid_tm: a real date from 1970 on, to the second. */
static int rtc_set(const struct lx_rtc_time *tm)
{
    int64_t y = (int64_t)tm->tm_year + 1900;
    if (tm->tm_year < 70 || tm->tm_mon < 0 || tm->tm_mon > 11 || tm->tm_mday < 1 ||
        (unsigned)tm->tm_mday > days_in_month(y, (unsigned)tm->tm_mon + 1) || tm->tm_hour < 0 ||
        tm->tm_hour > 23 || tm->tm_min < 0 || tm->tm_min > 59 || tm->tm_sec < 0 || tm->tm_sec > 59)
        return -EINVAL;
    uint64_t s = (uint64_t)time_days_from_civil(y, (unsigned)tm->tm_mon + 1, (unsigned)tm->tm_mday) * 86400ull +
                 (uint64_t)tm->tm_hour * 3600u + (uint64_t)tm->tm_min * 60u + (uint64_t)tm->tm_sec;
    return arch_rtc_write_epoch(s);
}

static int64_t rtc_ioctl(struct vnode *vn, struct file *f, unsigned req, uint64_t arg)
{
    (void)vn;
    (void)f;
    struct lx_rtc_time tm;
    switch (req) {
    case LX_RTC_RD_TIME: {
        int rc = rtc_get(&tm);
        if (rc)
            return rc;
        return copy_to_user(arg, &tm, sizeof(tm)) ? -EFAULT : 0;
    }
    case LX_RTC_SET_TIME:
        if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
            return -EACCES;
        if (copy_from_user(&tm, arg, sizeof(tm)))
            return -EFAULT;
        return rtc_set(&tm);
    default:
        return -ENOTTY;
    }
}

/* Linux's RTC device reads update interrupts, which this one has none
 * of: a read is refused rather than left to wait for one. */
static int64_t rtc_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    (void)buf;
    (void)len;
    return -EINVAL;
}

static int64_t rtc_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    (void)buf;
    (void)len;
    return -EINVAL;
}

static const struct chrdev_ops rtc_ops = { .read = rtc_read, .write = rtc_write, .ioctl = rtc_ioctl };

void rtcdev_init(void)
{
    if (!arch_rtc_present())
        return;   /* no RTC: no node, and hwclock says it cannot open one */
    int rc = ramfs_mkchr("/dev/rtc0", 0600, &rtc_ops, NULL, NULL);
    if (rc)
        kerror("rtc: /dev/rtc0: %d", rc);
    else
        kinfo("rtc: /dev/rtc0 ready (0600)");
}
