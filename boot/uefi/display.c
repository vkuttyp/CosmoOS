/*
 * display.c - The display mode the loader hands over: the one the display
 * prefers (docs/boot/design.md, "The display mode").
 *
 * The firmware leaves the Graphics Output Protocol in some mode; on
 * Parallels Desktop that is 1024x768 inside a window that is larger, so
 * the screen is a small picture in the middle of black. A display says
 * what it prefers in its EDID, whose first detailed timing is the native
 * mode; when the firmware offers that mode with a linear framebuffer, the
 * loader sets it. Otherwise the current mode stays, as before.
 */

#include "display_pick.h"
#include "loader.h"

/* What was found, for display_report after ExitBootServices: the lines
 * printed here go to the firmware's console, which the kernel's screen
 * console then overwrites. */
static char g_report[512];
static unsigned g_report_len;

static void note(const char *s)
{
    while (*s && g_report_len + 1 < sizeof(g_report))
        g_report[g_report_len++] = *s++;
    g_report[g_report_len] = '\0';
}

static void note_num(uint32_t v)
{
    char buf[11];
    unsigned n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof(buf));
    char out[12];
    for (unsigned i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    out[n] = '\0';
    note(out);
}

static void note_mode(uint32_t w, uint32_t h)
{
    note_num(w);
    note("x");
    note_num(h);
}

void display_report(void)
{
    if (g_report_len)
        lprintf("display: %s\n", g_report);
}

/* The Graphics Output Protocol the loader sets a mode on and hands over:
 * the first handle carrying one with a mode, and that handle in *handle,
 * so the EDID read is the same output's. Both display_choose_mode and the
 * framebuffer handoff take it from here, so they agree when the firmware
 * has more than one display. NULL when there is none. */
EFI_GRAPHICS_OUTPUT_PROTOCOL *display_gop(EFI_HANDLE *handle)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *found = NULL;
    *handle = NULL;
    if (EFI_ERROR(g_bs->LocateHandleBuffer(ByProtocol, &gop_guid, NULL, &count, &handles)) || handles == NULL)
        return NULL;
    for (UINTN i = 0; i < count && found == NULL; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        if (!EFI_ERROR(g_bs->HandleProtocol(handles[i], &gop_guid, (void **)&gop)) && gop != NULL &&
            gop->Mode != NULL && gop->Mode->Info != NULL) {
            found = gop;
            *handle = handles[i];
        }
    }
    g_bs->FreePool(handles);
    return found;
}

/* The EDID of the output on `handle`, active (what the output was set up
 * with) before discovered (what the display reported). An EDID on another
 * handle describes another output, and is not looked at. */
static bool find_edid(EFI_HANDLE handle, uint32_t *w, uint32_t *h)
{
    EFI_GUID guids[2] = { EFI_EDID_ACTIVE_PROTOCOL_GUID, EFI_EDID_DISCOVERED_PROTOCOL_GUID };
    for (unsigned i = 0; i < 2; i++) {
        EFI_EDID_PROTOCOL *edid = NULL;
        if (!EFI_ERROR(g_bs->HandleProtocol(handle, &guids[i], (void **)&edid)) && edid != NULL &&
            edid_preferred(edid->Edid, edid->SizeOfEdid, w, h))
            return true;
    }
    return false;
}

void display_choose_mode(void)
{
    EFI_HANDLE handle;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = display_gop(&handle);
    if (gop == NULL || gop->QueryMode == NULL || gop->SetMode == NULL)
        return;
    uint32_t cur_mode = gop->Mode->Mode;
    uint32_t cur_w = gop->Mode->Info->HorizontalResolution, cur_h = gop->Mode->Info->VerticalResolution;
    uint32_t want_w = 0, want_h = 0;
    bool have_edid = find_edid(handle, &want_w, &want_h);

    /* Every mode the firmware offers, for the record; the preferred one, if
     * it is there with a linear framebuffer. */
    uint32_t pick = UINT32_MAX;
    lprintf("display: %u mode(s), current %u (%ux%u)", gop->Mode->MaxMode, gop->Mode->Mode, cur_w, cur_h);
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN size = 0;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info)) || info == NULL)
            continue;
        uint32_t w = info->HorizontalResolution, h = info->VerticalResolution;
        bool linear = info->PixelFormat != PixelBltOnly;
        lprintf("%s %ux%u%s", m == 0 ? ":" : ",", w, h, linear ? "" : " (blt only)");
        note(m == 0 ? "modes " : " ");
        note_mode(w, h);
        const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *one[1] = { info };
        if (have_edid && pick == UINT32_MAX && display_pick(one, 1, want_w, want_h) == 0)
            pick = m;   /* the first offered whose pixels the handoff can describe */
        g_bs->FreePool(info);
    }
    lputs("\n");
    note("; firmware's ");
    note_mode(cur_w, cur_h);
    if (have_edid) {
        note("; EDID prefers ");
        note_mode(want_w, want_h);
    }
    if (!have_edid) {
        note("; no EDID: kept");
        lputs("display: no EDID; the firmware's mode stays\n");
        return;
    }
    if (pick == UINT32_MAX) {
        note(": not offered, kept");
        lprintf("display: EDID prefers %ux%u, which the firmware does not offer; the firmware's mode stays\n",
                want_w, want_h);
        return;
    }
    if (pick == gop->Mode->Mode) {
        note(": already set");
        lprintf("display: EDID prefers %ux%u, the current mode\n", want_w, want_h);
        return;
    }
    EFI_STATUS st = gop->SetMode(gop, pick);
    if (EFI_ERROR(st)) {
        note(": SetMode failed, kept");
        lprintf("display: setting %ux%u failed (0x%llx); the firmware's mode stays\n", want_w, want_h,
                (unsigned long long)st);
        return;
    }
    /* What the handoff will check (find_framebuffer): a mode whose memory
     * the firmware does not describe big enough is no use to the kernel,
     * so the firmware's mode is put back rather than handing over none. */
    struct display_format f;
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
    uint64_t per_line = mi && mi->PixelsPerScanLine ? mi->PixelsPerScanLine : want_w;
    if (mi == NULL || !display_format(mi, &f) || gop->Mode->FrameBufferBase == 0 ||
        per_line * (f.bpp / 8) * mi->VerticalResolution > gop->Mode->FrameBufferSize) {
        gop->SetMode(gop, cur_mode);
        note(": unusable once set, restored");
        lprintf("display: %ux%u set but its framebuffer is unusable; back to %ux%u\n", want_w, want_h, cur_w,
                cur_h);
        return;
    }
    note(": set");
    lprintf("display: %ux%u, as the EDID prefers (was %ux%u)\n", want_w, want_h, cur_w, cur_h);
}
