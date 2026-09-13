/*
 * cwdtest - the current directory, from more than one thread
 * (docs/audit/next-subsystem-cwd-ref.md).
 *
 * Every relative-path system call resolves against the process's current
 * directory. Until this unit they all read `p->cwd` with no lock and no
 * reference and handed the raw pointer to a path walk, while another
 * thread's `chdir` could swap it and drop the last reference -- freeing a
 * vnode a walk was inside. This program is the race that finds that.
 *
 * **Every move here is relative**, and that is a requirement rather than
 * a style. An absolute `chdir` bypasses the normalisation base entirely,
 * so a torn `cwd_path` is never read and `getcwd` already copies under
 * the lock: written with absolute paths, step 2 passes against the
 * unfixed kernel and proves nothing. The layout exists to make relative
 * moves always valid:
 *
 *     /tmp/cwdr/a   with a file `is-a`
 *     /tmp/cwdr/b   with a file `is-b`
 *
 * From either directory, `../a` and `../b` are both real, so no move can
 * fail for an ordinary reason and hide the race behind an -ENOENT.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cosmo/syscall.h>
#include <cosmo/thread.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("cwdtest: FAIL %s at line %d\n", #cond, __LINE__);        \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define STEP(n)                                                              \
    do {                                                                     \
        printf("cwdtest: step %s\n", (n));                                   \
        fflush(stdout);                                                      \
    } while (0)

#define ROUNDS 4000u

/*
 * The names are long, different from each other in every byte, and of
 * **different lengths** -- all three on purpose.
 *
 * A first version used `/tmp/cwdr/a` and `/tmp/cwdr/b`, which cannot
 * detect a torn `cwd_path` at all: the two strings differ in exactly one
 * byte, so any mixture of them is still one of the two valid answers, and
 * a single-byte store is atomic anyway. The proof for the torn-buffer
 * defect passed against the unfixed kernel because of it.
 *
 * With these, a copy interrupted part-way leaves the head of one name and
 * the tail of the other -- a string that is neither, and visibly so.
 */
#define NAME_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NAME_B "bbbb"
static const char *DIR_A = "/tmp/cwdr/" NAME_A;
static const char *DIR_B = "/tmp/cwdr/" NAME_B;

/*
 * The writers and the observer meet here. `parked` lets the observer stop
 * the writers before it looks at the process's directory -- see step 3 for
 * why a check that runs while they are moving cannot be trusted.
 */
static cosmo_mutex_t m = COSMO_MUTEX_INIT;
static cosmo_cond_t c = COSMO_COND_INIT;
static volatile unsigned quiesce;      /* writers: park at the top of the loop */
static volatile unsigned parked;       /* how many are parked */
static volatile unsigned writers_stop;
static volatile unsigned bad_errno;    /* an open failed for something other than -ENOENT */
static volatile unsigned bad_path;     /* getcwd answered neither directory */
static volatile unsigned opens_ok, opens_enoent;

static void writer_checkpoint(void)
{
    cosmo_mutex_lock(&m);
    while (quiesce) {
        parked++;
        cosmo_cond_broadcast(&c);
        while (quiesce)
            cosmo_cond_wait(&c, &m);
        parked--;
        cosmo_cond_broadcast(&c);
    }
    cosmo_mutex_unlock(&m);
}

/* Alternate between the two directories, relatively. */
static void *chdir_writer(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        writer_checkpoint();
        if (chdir("../" NAME_A) != 0 && chdir("../" NAME_B) != 0)
            continue;    /* a move may lose a race with the other writer; both are valid */
        writer_checkpoint();
        (void)chdir("../" NAME_B);
    }
    cosmo_mutex_lock(&m);
    writers_stop = 1;
    cosmo_cond_broadcast(&c);
    cosmo_mutex_unlock(&m);
    return NULL;
}

/*
 * Open a relative path while the directory moves under it. The file exists
 * in one directory and not the other, so **either answer is correct** --
 * what must never happen is any *other* errno, and what must never happen
 * at all is a fault, which is the bug this program is named for and which
 * the boot test sees as a dead process rather than as a failed check.
 */
static void *open_reader(void *arg)
{
    (void)arg;
    while (!writers_stop) {
        int fd = open("is-a", O_RDONLY);
        if (fd >= 0) {
            opens_ok++;
            close(fd);
        } else if (errno == ENOENT) {
            opens_enoent++;
        } else {
            bad_errno = (unsigned)errno;
            break;
        }
    }
    return NULL;
}

/* --- step 2: a move that keeps the base -----------------------------------
 *
 * `../X` cannot show a torn `cwd_path`, and finding that out took a run:
 * `..` discards the last component, which is the *only* part where two
 * sibling names differ. Whatever mixture of them the reader saw, the
 * result was the same either way.
 *
 * So step 2 moves **down** instead. A plain `chdir("s")` keeps the whole
 * base and appends to it, so every differing byte survives into the
 * answer. The two bases are at different depths and share nothing after
 * `/tmp/cwdr/`:
 *
 *     /tmp/cwdr/pppppppppppppppppppppppppppppppp/s
 *     /tmp/cwdr/q/s
 *
 * Each writer walks its own pair down and up. A reader that catches the
 * buffer mid-copy normalises against the head of one and the tail of the
 * other, and `getcwd` then answers a path that is neither. */
#define NAME_P "pppppppppppppppppppppppppppppppp"
#define NAME_Q "q"
#define P_TOP "/tmp/cwdr/" NAME_P
#define P_SUB "/tmp/cwdr/" NAME_P "/s"
#define Q_TOP "/tmp/cwdr/" NAME_Q
#define Q_SUB "/tmp/cwdr/" NAME_Q "/s"

static void *depth_writer(void *arg)
{
    /* 0 or 1 rather than the string itself: a string literal is `const
     * char *`, and casting the const away to fit `void *` is a -Werror
     * here and a lie anywhere. */
    const char *top = (unsigned long)arg ? Q_TOP : P_TOP;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        if (chdir(top) != 0)
            continue;
        (void)chdir("s");     /* down: keeps the whole base, so a tear shows */
        (void)chdir("..");
    }
    writers_stop = 1;
    return NULL;
}

/* Every answer must be one of the four real paths. A torn base produces a
 * fifth. */
static void *depth_observer(void *arg)
{
    (void)arg;
    char buf[256];
    while (!writers_stop) {
        if (getcwd(buf, sizeof(buf)) == NULL) {
            bad_path++;
            break;
        }
        if (strcmp(buf, P_TOP) && strcmp(buf, P_SUB) &&
            strcmp(buf, Q_TOP) && strcmp(buf, Q_SUB) &&
            strcmp(buf, "/tmp/cwdr")) {
            printf("cwdtest: getcwd answered '%s'\n", buf);
            bad_path++;
            break;
        }
    }
    return NULL;
}

/* Stop the writers and wait until every one of them is parked. */
static void quiesce_writers(unsigned n)
{
    cosmo_mutex_lock(&m);
    quiesce = 1;
    cosmo_cond_broadcast(&c);
    while (parked < n && !writers_stop)
        cosmo_cond_wait(&c, &m);
    cosmo_mutex_unlock(&m);
}

static void release_writers(void)
{
    cosmo_mutex_lock(&m);
    quiesce = 0;
    cosmo_cond_broadcast(&c);
    cosmo_mutex_unlock(&m);
}

/* --- step 4: the victim directory ---------------------------------------- */
#define VICTIM "vic"
static volatile unsigned vic_walks, vic_frees_seen;

/* Move the process in and out of the victim. The `chdir("..")` is the put
 * that can be the last one, once the killer has removed the entry. */
static void *vic_mover(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < ROUNDS && !writers_stop; i++) {
        if (chdir(VICTIM) == 0)
            (void)chdir("..");
    }
    writers_stop = 1;
    return NULL;
}

/* Remove the entry, which drops ramfs's pinned reference, and put it back
 * so the mover has something to enter next time round. */
static void *vic_killer(void *arg)
{
    (void)arg;
    while (!writers_stop) {
        (void)rmdir("/tmp/cwdr/" VICTIM);
        (void)mkdir("/tmp/cwdr/" VICTIM, 0755);
    }
    return NULL;
}

/* Walk a relative path. When the process is inside the victim this
 * dereferences the victim's vnode -- with a reference of its own since
 * this unit, and with a bare pointer before it. */
static void *vic_walker(void *arg)
{
    (void)arg;
    char buf[256];
    while (!writers_stop) {
        int fd = open("f", O_RDONLY);
        if (fd >= 0) {
            close(fd);
        } else if (errno != ENOENT && errno != ENOTDIR) {
            bad_errno = (unsigned)errno;
            break;
        }
        if (getcwd(buf, sizeof(buf)) != NULL && strstr(buf, VICTIM) != NULL)
            vic_walks++;
    }
    return NULL;
}

static int make_layout(void)
{
    if (mkdir("/tmp/cwdr", 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(DIR_A, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(DIR_B, 0755) != 0 && errno != EEXIST)
        return -1;
    int fd = open("/tmp/cwdr/" NAME_A "/is-a", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    fd = open("/tmp/cwdr/" NAME_B "/is-b", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    /* Step 2's pair: different depths, no shared bytes after the root. */
    if (mkdir(P_TOP, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(P_SUB, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(Q_TOP, 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(Q_SUB, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(void)
{
    printf("cwdtest: start\n");
    fflush(stdout);
    CHECK(make_layout() == 0);
    CHECK(chdir(DIR_A) == 0);

    STEP("1");
    /*
     * (1) **A walk survives the directory moving under it.** One thread
     * alternates between the two directories, another opens a relative
     * path in a loop. Before this unit the opener held a pointer to a
     * vnode the writer could free.
     *
     * The observable is that the process is still alive and every refusal
     * was -ENOENT. A use-after-free here is a fault, which ends the
     * process -- so this step's real verdict is that the ones after it
     * run at all.
     */
    {
        cosmo_thread_t w, r;
        writers_stop = quiesce = parked = bad_errno = 0;
        opens_ok = opens_enoent = 0;
        CHECK(cosmo_thread_start(&w, chdir_writer, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&r, open_reader, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_join(&w, NULL) == 0);
        CHECK(cosmo_thread_join(&r, NULL) == 0);
        CHECK(bad_errno == 0);
        /* Both answers must actually have happened, or the threads did not
         * overlap and this step tested one directory in peace. */
        CHECK(opens_ok > 0);
        CHECK(opens_enoent > 0);
        printf("cwdtest: %u opens ok, %u enoent\n", opens_ok, opens_enoent);
    }

    STEP("2");
    /*
     * (2) **`chdir` racing `chdir`, with a reader of the name.** Two
     * writers moving relatively, one observer calling `getcwd`. The name
     * must always be exactly one of the two directories: `process_chdir`
     * normalises against `cwd_path`, and reading that buffer while another
     * thread is copying into it yields the head of one path and the tail
     * of another.
     */
    {
        cosmo_thread_t w1, w2, o;
        CHECK(chdir("/tmp/cwdr") == 0);
        writers_stop = quiesce = parked = bad_path = 0;
        CHECK(cosmo_thread_start(&w1, depth_writer, (void *)0ul, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&w2, depth_writer, (void *)1ul, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&o, depth_observer, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_join(&w1, NULL) == 0);
        CHECK(cosmo_thread_join(&w2, NULL) == 0);
        CHECK(cosmo_thread_join(&o, NULL) == 0);
        CHECK(bad_path == 0);
    }

    STEP("3");
    /*
     * (3) **The name and the directory agree.** `process_chdir` publishes
     * a path and a vnode; if it takes them from two acquisitions of the
     * lock rather than one snapshot, they can come from different
     * directories -- and the process is then left reporting one directory
     * through `getcwd` while resolving relative paths in another.
     *
     * **The writers are stopped first, and that is load-bearing.** The
     * obvious version of this check -- `getcwd`, then open the file that
     * belongs to it -- fails on a *correct* kernel, because a writer may
     * legitimately `chdir` between those two system calls. Quiescing makes
     * the comparison honest, and it works because the defect's damage is
     * **persistent, not transient**: a process left holding one
     * directory's path and another's vnode stays that way until its next
     * `chdir`.
     */
    {
        cosmo_thread_t w1, w2;
        char buf[256];
        CHECK(chdir(DIR_A) == 0);
        writers_stop = quiesce = parked = 0;
        CHECK(cosmo_thread_start(&w1, chdir_writer, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&w2, chdir_writer, NULL, 32u * 1024u) == 0);
        unsigned checked = 0;
        for (unsigned i = 0; i < 200u && !writers_stop; i++) {
            quiesce_writers(2);
            if (writers_stop)
                break;
            if (getcwd(buf, sizeof(buf)) != NULL) {
                const char *want = strcmp(buf, DIR_A) == 0 ? "is-a"
                                 : strcmp(buf, DIR_B) == 0 ? "is-b" : NULL;
                if (want == NULL) {
                    printf("cwdtest: getcwd answered '%s'\n", buf);
                    bad_path++;
                } else {
                    int fd = open(want, O_RDONLY);   /* relative: uses the vnode */
                    if (fd < 0) {
                        printf("cwdtest: getcwd said %s but '%s' is absent (errno %d)\n",
                               buf, want, errno);
                        bad_path++;
                    } else {
                        close(fd);
                        checked++;
                    }
                }
            }
            release_writers();
        }
        cosmo_mutex_lock(&m);
        writers_stop = 1;
        quiesce = 0;
        cosmo_cond_broadcast(&c);
        cosmo_mutex_unlock(&m);
        CHECK(cosmo_thread_join(&w1, NULL) == 0);
        CHECK(cosmo_thread_join(&w2, NULL) == 0);
        CHECK(bad_path == 0);
        CHECK(checked > 0);   /* the comparison actually ran */
        printf("cwdtest: %u coherent checks\n", checked);
    }

    STEP("4");
    /*
     * (4) **The directory is removed while a walk is inside it**, which is
     * what it actually takes to free the vnode here.
     *
     * Steps 1 to 3 cannot free anything, and finding that out is most of
     * what this step is for. `ramfs` -- which backs `/tmp` -- holds a
     * **pinned reference** from a directory entry to its child
     * (`kernel-services/vfs/ramfs.c:25`), so every directory that still
     * exists is referenced by its parent no matter what any `chdir` does.
     * A measurement said so before this step was written: zero directory
     * vnodes were freed during the whole of steps 1 to 3.
     *
     * So the last reference can only be the current directory once the
     * entry is gone. Three threads: one moving the process in and out of
     * `vic`, one removing and recreating `vic`, and one walking a relative
     * path. When `vic` has been removed and the mover leaves it, the put
     * is the last -- and a walker that kept no reference of its own is
     * inside a freed vnode.
     */
    {
        cosmo_thread_t mv, kl, wk;
        CHECK(chdir("/tmp/cwdr") == 0);
        (void)mkdir(VICTIM, 0755);
        writers_stop = quiesce = parked = bad_errno = 0;
        vic_walks = vic_frees_seen = 0;
        CHECK(cosmo_thread_start(&mv, vic_mover, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&kl, vic_killer, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_start(&wk, vic_walker, NULL, 32u * 1024u) == 0);
        CHECK(cosmo_thread_join(&mv, NULL) == 0);
        CHECK(cosmo_thread_join(&kl, NULL) == 0);
        CHECK(cosmo_thread_join(&wk, NULL) == 0);
        CHECK(bad_errno == 0);
        CHECK(vic_walks > 0);
        printf("cwdtest: %u walks in the victim\n", vic_walks);
        CHECK(chdir("/tmp/cwdr") == 0);
        (void)mkdir(VICTIM, 0755);
    }

    CHECK(chdir("/") == 0);
    printf("cwdtest: %d failure(s)\n", failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
