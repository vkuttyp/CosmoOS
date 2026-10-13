/*
 * firmware.c - What the firmware offers the kernel besides tables: a seed
 * from EFI_RNG_PROTOCOL and the time from GetTime (boot protocol v10;
 * docs/boot/design.md, "Firmware seed and time").
 *
 * Both are read just before ExitBootServices, after every allocation, so
 * the time is as late as the loader can make it. Neither is required: a
 * firmware without them leaves the fields zero, and says so.
 */

#include "loader.h"
#include "cosmoboot.h"

/* Days from 1970-01-01 to the civil date (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static unsigned days_in_month(unsigned y, unsigned m)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29u : days[m - 1];
}

static void fw_time(struct cosmoboot_info *info)
{
    EFI_RUNTIME_SERVICES_HEAD *rt = g_st->RuntimeServices;
    EFI_TIME t;
    EFI_STATUS st = rt != NULL && rt->GetTime != NULL ? rt->GetTime(&t, NULL) : EFI_UNSUPPORTED;
    if (EFI_ERROR(st)) {
        lprintf("firmware: no time (GetTime %s)\n", rt == NULL || rt->GetTime == NULL ? "absent" : "failed");
        return;
    }
    /* Up to 2500: the kernel keeps the wall clock in 64-bit nanoseconds,
     * which end in 2554. */
    if (t.Year < 1970 || t.Year > 2500 || t.Month < 1 || t.Month > 12 || t.Day < 1 ||
        t.Day > days_in_month(t.Year, t.Month) || t.Hour > 23 || t.Minute > 59 || t.Second > 59) {
        lprintf("firmware: GetTime gave an impossible date %u-%u-%u %u:%u:%u; not used\n", t.Year, t.Month, t.Day,
                t.Hour, t.Minute, t.Second);
        return;
    }
    int64_t s = days_from_civil(t.Year, t.Month, t.Day) * 86400 + t.Hour * 3600 + t.Minute * 60 + t.Second;
    /* UEFI: local time = UTC - TimeZone (minutes); unspecified means the
     * clock is taken as UTC. */
    if (t.TimeZone != EFI_UNSPECIFIED_TIMEZONE && t.TimeZone >= -1440 && t.TimeZone <= 1440)
        s += (int64_t)t.TimeZone * 60;
    info->fw_epoch = s;
    info->fw_flags |= COSMOBOOT_FW_TIME;
    lprintf("firmware: time %04u-%02u-%02u %02u:%02u:%02u (zone %s)\n", t.Year, t.Month, t.Day, t.Hour, t.Minute,
            t.Second, t.TimeZone == EFI_UNSPECIFIED_TIMEZONE ? "unspecified, taken as UTC" : "given");
}

static void fw_seed(struct cosmoboot_info *info)
{
    EFI_GUID guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;
    EFI_STATUS st = g_bs->LocateProtocol(&guid, NULL, (void **)&rng);
    if (EFI_ERROR(st) || rng == NULL) {
        lputs("firmware: no EFI_RNG_PROTOCOL: no seed\n");
        return;
    }
    st = rng->GetRNG(rng, NULL, COSMOBOOT_FW_SEED_MAX, info->fw_seed);
    if (EFI_ERROR(st)) {
        memset(info->fw_seed, 0, sizeof(info->fw_seed));
        lprintf("firmware: EFI_RNG_PROTOCOL failed (0x%llx): no seed\n", (unsigned long long)st);
        return;
    }
    info->fw_seed_len = COSMOBOOT_FW_SEED_MAX;
    lprintf("firmware: %u-byte seed from EFI_RNG_PROTOCOL\n", (unsigned)COSMOBOOT_FW_SEED_MAX);
}

void firmware_gather(struct cosmoboot_info *info)
{
    fw_seed(info);
    fw_time(info);
}

void firmware_report(const struct cosmoboot_info *info)
{
    /* The lines above went to the firmware's console, which the kernel's
     * framebuffer console then overwrites; the serial port keeps these. */
    lprintf("firmware: seed %u bytes; time %s\n", (unsigned)info->fw_seed_len,
            (info->fw_flags & COSMOBOOT_FW_TIME) ? "from GetTime" : "none");
}
