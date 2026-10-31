/* convert.c - Pure Linux/native conversions (compiled in the kernel and by tests/host/test_linux.c): the encoders
 * whose Linux layout differs between the architectures (stat, statx) and the terminal's modes out. The decoders and
 * the rest are lxconv's (roadmap 2.0 SA5). */

#include <kernel/errno.h>
#include <kernel/string.h>
#include <stddef.h>

#include "convert.h"

/* Linux's makedev (glibc's and musl's encoding): the minor's low byte, the
 * major's twelve bits, then the minor's rest. A file's device is
 * makedev(0, its mount's id). */
static uint64_t lx_makedev(uint32_t major, uint32_t minor)
{
    return ((uint64_t)(major & 0xFFFFF000u) << 32) | ((uint64_t)(major & 0xFFFu) << 8) |
           ((uint64_t)(minor & 0xFFFFFF00u) << 12) | (minor & 0xFFu);
}

void lx_stat_from_native(const struct cosmo_stat *st, struct lx_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = lx_makedev(0, st->dev);
    out->st_ino = st->ino;
    out->st_nlink = st->nlink;
    uint32_t type;
    switch (st->type) {
    case COSMO_DT_DIR: type = LX_S_IFDIR; break;
    case COSMO_DT_CHR: type = LX_S_IFCHR; break;
    case COSMO_DT_FIFO: type = LX_S_IFIFO; break;
    case COSMO_DT_SOCK: type = LX_S_IFSOCK; break;
    case COSMO_DT_LNK: type = LX_S_IFLNK; break;
    default: type = LX_S_IFREG; break;
    }
    out->st_mode = type | (st->mode & 07777u);
    out->st_uid = st->uid;
    out->st_gid = st->gid;
    out->st_size = (int64_t)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((st->size + 511) / 512);
    out->st_mtime = (int64_t)(st->mtime_ns / 1000000000ull);
    out->st_mtime_nsec = (int64_t)(st->mtime_ns % 1000000000ull);
    out->st_ctime = (int64_t)(st->ctime_ns / 1000000000ull);
    out->st_ctime_nsec = (int64_t)(st->ctime_ns % 1000000000ull);
    out->st_atime = out->st_mtime;
    out->st_atime_nsec = out->st_mtime_nsec;
}

_Static_assert(sizeof(struct lx_statx) == LX_STATX_BYTES, "statx must be 256 bytes on both ABIs");

void lx_statx_from_native(const struct cosmo_stat *st, uint32_t mask, struct lx_statx *out)
{
    (void)mask;   /* advisory in Linux; we report the set we actually fill */
    memset(out, 0, sizeof(*out));
    uint32_t type;
    switch (st->type) {
    case COSMO_DT_DIR: type = LX_S_IFDIR; break;
    case COSMO_DT_CHR: type = LX_S_IFCHR; break;
    case COSMO_DT_FIFO: type = LX_S_IFIFO; break;
    case COSMO_DT_SOCK: type = LX_S_IFSOCK; break;
    case COSMO_DT_LNK: type = LX_S_IFLNK; break;
    default: type = LX_S_IFREG; break;
    }
    out->stx_mode = (uint16_t)(type | (st->mode & 07777u));
    out->stx_nlink = (uint32_t)st->nlink;
    out->stx_uid = st->uid;
    out->stx_gid = st->gid;
    out->stx_ino = st->ino;
    out->stx_dev_major = 0;
    out->stx_dev_minor = st->dev;   /* makedev(0, dev): what st_dev says */
    out->stx_size = st->size;
    out->stx_blksize = 4096;
    out->stx_blocks = (st->size + 511) / 512;
    out->stx_mtime.tv_sec = (int64_t)(st->mtime_ns / 1000000000ull);
    out->stx_mtime.tv_nsec = (uint32_t)(st->mtime_ns % 1000000000ull);
    out->stx_ctime.tv_sec = (int64_t)(st->ctime_ns / 1000000000ull);
    out->stx_ctime.tv_nsec = (uint32_t)(st->ctime_ns % 1000000000ull);
    /* atime and btime are left zero AND out of the mask: the kernel records
     * neither, and statx says a field is meaningful only when its bit is set
     * -- so the caller is told they are absent, not handed a value. */
    out->stx_mask = LX_STATX_SUPPORTED;
}

/*
 * The terminal's modes, both ways. Linux's structure has four flag
 * words and nineteen control characters; this kernel has four flags and
 * two numbers, so the translation is mostly deciding what to say about
 * the fields that do not exist. It says the truth: the bits this tree
 * implements are reported as they are, and the rest read as a plausible
 * quiet terminal rather than as zero, because a libc that sees c_oflag
 * with OPOST clear will stop translating newlines itself and the
 * console does that translation in its sink.
 */
void lx_termios_from_native(struct lx_termios *out, const struct cosmo_termios *in)
{
    memset(out, 0, sizeof(*out));
    out->c_iflag = (in->modes & COSMO_TTY_ICRNL) ? LX_ICRNL : 0;
    /* OPOST|ONLCR: the newline goes out as CR LF -- by a pty's output
     * processing, or by the console's serial sink, which does it whatever
     * the mode says. Reporting it clear would make a libc translate too. */
    out->c_oflag = (in->modes & COSMO_TTY_OPOST) ? (LX_OPOST | LX_ONLCR) : 0;
    /* A terminal that says 8 bits, receiver on, 38400 baud: there is no
     * line to describe, and zero would read as a hung-up modem (B0). */
    out->c_cflag = LX_CS8 | LX_CREAD | LX_B38400;
    out->c_lflag = ((in->modes & COSMO_TTY_ISIG) ? LX_ISIG : 0) |
                   ((in->modes & COSMO_TTY_ICANON) ? LX_ICANON : 0) |
                   ((in->modes & COSMO_TTY_ECHO) ? (LX_ECHO | LX_ECHOE | LX_ECHOK | LX_ECHOCTL | LX_ECHOKE) : 0) |
                   ((in->modes & COSMO_TTY_TOSTOP) ? LX_TOSTOP : 0) | LX_IEXTEN;
    /* The control characters this line discipline acts on, at Linux's
     * defaults (they are fixed here: setting another is accepted and has
     * no effect). */
    out->c_cc[LX_VINTR] = 0x03;
    out->c_cc[LX_VQUIT] = 0x1c;
    out->c_cc[LX_VERASE] = 0x7f;
    out->c_cc[LX_VKILL] = 0x15;
    out->c_cc[LX_VEOF] = 0x04;
    out->c_cc[LX_VSTART] = 0x11;
    out->c_cc[LX_VSTOP] = 0x13;
    out->c_cc[LX_VSUSP] = 0x1a;
    out->c_cc[LX_VREPRINT] = 0x12;
    out->c_cc[LX_VDISCARD] = 0x0f;
    out->c_cc[LX_VWERASE] = 0x17;
    out->c_cc[LX_VLNEXT] = 0x16;
    out->c_cc[LX_VMIN] = in->vmin;
    out->c_cc[LX_VTIME] = in->vtime;
}

