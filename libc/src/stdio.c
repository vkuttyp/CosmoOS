/* stdio.c - Buffered streams (docs/libc/design.md). */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libc.h"

#define F_READ    (1u << 0)
#define F_WRITE   (1u << 1)
#define F_EOF     (1u << 2)
#define F_ERR     (1u << 3)
#define F_LINEBUF (1u << 4)
#define F_UNBUF   (1u << 5)
#define F_READING (1u << 6)   /* the buffer holds input */
#define F_WRITING (1u << 7)   /* the buffer holds output */
#define F_STATIC  (1u << 8)   /* one of the three standard streams */

struct _FILE {
    int fd;
    unsigned flags;
    unsigned char *buf;
    size_t cap;
    size_t len;    /* valid bytes (reading) or pending bytes (writing) */
    size_t pos;    /* read position */
    int ungot;     /* -1 when empty */
    struct _FILE *next;
};

static unsigned char g_bufs[3][BUFSIZ];
static struct _FILE g_std[3];
FILE *stdin = &g_std[0];
FILE *stdout = &g_std[1];
FILE *stderr = &g_std[2];
static struct _FILE *g_files;

static void file_init(struct _FILE *f, int fd, unsigned flags, unsigned char *buf, size_t cap)
{
    f->fd = fd;
    f->flags = flags;
    f->buf = buf;
    f->cap = cap;
    f->len = f->pos = 0;
    f->ungot = -1;
    f->next = g_files;
    g_files = f;
}

void __stdio_init(void)
{
    file_init(&g_std[0], 0, F_READ | F_STATIC, g_bufs[0], BUFSIZ);
    file_init(&g_std[1], 1, F_WRITE | F_LINEBUF | F_STATIC, g_bufs[1], BUFSIZ);
    file_init(&g_std[2], 2, F_WRITE | F_UNBUF | F_STATIC, g_bufs[2], BUFSIZ);
}

static int flush_out(FILE *f)
{
    size_t done = 0;
    while (done < f->len) {
        ssize_t n = write(f->fd, f->buf + done, f->len - done);
        if (n <= 0) {
            f->flags |= F_ERR;
            f->len = 0;
            return EOF;
        }
        done += (size_t)n;
    }
    f->len = 0;
    f->flags &= ~F_WRITING;
    return 0;
}

int fflush(FILE *f)
{
    if (f == NULL) {
        __stdio_flush_all();
        return 0;
    }
    if (f->flags & F_WRITING)
        return flush_out(f);
    if (f->flags & F_READING) {
        f->len = f->pos = 0;
        f->flags &= ~F_READING;
        f->ungot = -1;
    }
    return 0;
}

void __stdio_flush_all(void)
{
    for (struct _FILE *f = g_files; f; f = f->next)
        if (f->flags & F_WRITING)
            flush_out(f);
}

static int parse_mode(const char *mode, unsigned *flags)
{
    int oflags;
    *flags = 0;
    switch (mode[0]) {
    case 'r':
        oflags = O_RDONLY;
        *flags = F_READ;
        break;
    case 'w':
        oflags = O_WRONLY | O_CREAT | O_TRUNC;
        *flags = F_WRITE;
        break;
    case 'a':
        oflags = O_WRONLY | O_CREAT | O_APPEND;
        *flags = F_WRITE;
        break;
    default:
        return -1;
    }
    if (strchr(mode, '+')) {
        oflags = (oflags & ~O_ACCMODE) | O_RDWR;
        *flags = F_READ | F_WRITE;
    }
    return oflags;
}

FILE *fdopen(int fd, const char *mode)
{
    unsigned flags;
    if (parse_mode(mode, &flags) < 0) {
        errno = EINVAL;
        return NULL;
    }
    FILE *f = malloc(sizeof(*f));
    unsigned char *buf = malloc(BUFSIZ);
    if (f == NULL || buf == NULL) {
        free(f);
        free(buf);
        return NULL;
    }
    file_init(f, fd, flags, buf, BUFSIZ);
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    unsigned flags;
    int oflags = parse_mode(mode, &flags);
    if (oflags < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, oflags, 0644);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, mode);
    if (f == NULL)
        close(fd);
    return f;
}

int fclose(FILE *f)
{
    int rc = fflush(f);
    if (close(f->fd) < 0)
        rc = EOF;
    struct _FILE **pp = &g_files;
    while (*pp && *pp != f)
        pp = &(*pp)->next;
    if (*pp)
        *pp = f->next;
    if (!(f->flags & F_STATIC)) {
        free(f->buf);
        free(f);
    }
    return rc;
}

/* --- input --- */

static int fill(FILE *f)
{
    if (!(f->flags & F_READ)) {
        f->flags |= F_ERR;
        return EOF;
    }
    if (f->flags & F_WRITING)
        flush_out(f);
    f->flags |= F_READING;
    ssize_t n = read(f->fd, f->buf, f->cap);
    if (n < 0) {
        f->flags |= F_ERR;
        return EOF;
    }
    if (n == 0) {
        f->flags |= F_EOF;
        return EOF;
    }
    f->len = (size_t)n;
    f->pos = 0;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        return c;
    }
    if (f->pos >= f->len && fill(f) < 0)
        return EOF;
    return f->buf[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || f->ungot >= 0)
        return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~F_EOF;
    return f->ungot;
}

char *fgets(char *s, int n, FILE *f)
{
    if (n <= 0)
        return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = '\0';
    return s;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n, got = 0;
    unsigned char *out = buf;
    while (got < total) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        out[got++] = (unsigned char)c;
    }
    return size ? got / size : 0;
}

/* --- output --- */

int fputc(int c, FILE *f)
{
    if (!(f->flags & F_WRITE)) {
        f->flags |= F_ERR;
        return EOF;
    }
    if (f->flags & F_READING)
        fflush(f);
    f->flags |= F_WRITING;
    if (f->len == f->cap && flush_out(f) < 0)
        return EOF;
    f->buf[f->len++] = (unsigned char)c;
    if ((f->flags & F_UNBUF) || ((f->flags & F_LINEBUF) && c == '\n'))
        if (flush_out(f) < 0)
            return EOF;
    return (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    const unsigned char *in = buf;
    size_t total = size * n;
    for (size_t i = 0; i < total; i++)
        if (fputc(in[i], f) == EOF)
            return size ? i / size : 0;
    return n;
}

int fputs(const char *s, FILE *f)
{
    return fwrite(s, 1, strlen(s), f) == strlen(s) ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

/* --- status and position --- */

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

int fseek(FILE *f, long off, int whence)
{
    /* The descriptor sits past the buffered input; a relative seek is
     * meant from the logical position. */
    if (whence == SEEK_CUR && (f->flags & F_READING))
        off -= (long)(f->len - f->pos) + (f->ungot >= 0 ? 1 : 0);
    if (fflush(f) < 0)
        return -1;
    f->ungot = -1;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    if (f->flags & F_READING)
        return pos - (long)(f->len - f->pos) - (f->ungot >= 0 ? 1 : 0);
    return pos + (long)f->len;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

int remove(const char *path)
{
    return unlink(path) == 0 ? 0 : (errno == EISDIR ? rmdir(path) : -1);
}
