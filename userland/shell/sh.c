/*
 * sh.c - The CosmoOS shell (docs/userland/design.md).
 *
 * Reads lines, tokenises them (words with single/double quotes and
 * backslash escapes, '#' comments), expands $VAR ${VAR} $? $$ $0..$9 $#,
 * parses lists of pipelines with redirections, and runs them: builtins in
 * this process, programs through spawn with an explicit handle map.
 * There is no fork: pipelines are built by spawning every stage with its
 * pipe ends mapped, then waiting for all of them.
 *
 * No control flow, globbing, background jobs or command substitution in
 * this phase; the structures are shaped so they slot in.
 *
 * An interactive shell also runs the terminal: it starts a session of
 * its own, puts each pipeline in a process group of its own, and hands
 * the terminal to that group while it runs. That is what makes ^C
 * interrupt the command and not the shell -- the kernel sends SIGINT to
 * the terminal's foreground group, and the shell has arranged for that
 * to be the job (docs/kernel/process/design.md, "Sessions and process
 * groups").
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_MAX_  1024
#define WORDS_MAX  128
#define TOKENS_MAX 256
#define STAGES_MAX 16
#define REDIRS_MAX 8
#define VARS_MAX   64

/* --- state --- */

static int g_last_status;
static int g_opt_errexit;
static int g_interactive;
static int g_job_control;        /* the terminal is this shell's to hand out */
static pid_t g_shell_pgrp;

/*
 * Jobs. One pipeline is one job and therefore one process group, so a
 * job is remembered by its group and the stages in it: `fg` needs the
 * group to hand the terminal to and the pids to wait for, and `jobs`
 * needs the text to print. A job is forgotten when every stage has
 * exited.
 */
#define JOBS_MAX 16
/* What `job_wait_foreground` answers when the job stopped rather than
 * finished. 148 is 128 + SIGTSTP, which is what `$?` is after a ^Z in
 * every shell -- but it must not be mistaken for a death, so the caller
 * tests for it by name before reporting a signal. */
#define JOB_STOPPED (128 + COSMO_SIGTSTP)
struct job {
    int used;                /* the slot is in use */
    int id;                  /* what %n names; 0 until the job outlives the foreground */
    pid_t pgrp;
    pid_t pids[STAGES_MAX];
    int npids;
    int done[STAGES_MAX];
    int parked[STAGES_MAX];  /* reported stopped; cleared when continued */
    int stopped;             /* the whole job is parked */
    int reported;            /* the shell has told the user about its current state */
    int last_status;
    char text[128];          /* the command line, for `jobs` */
};
static struct job g_jobs[JOBS_MAX];
static int g_next_job_id = 1;
static int g_current_job;    /* the `%%` / `%+` one: what plain `fg` means */
static const char *g_script_name = "sh";
static char **g_script_args;     /* $1.. */
static int g_script_argc;

struct var {
    char *name;
    char *value;
};
static struct var g_vars[VARS_MAX];
static int g_nvars;

/* --- tokens --- */

enum tok_type { T_WORD, T_PIPE, T_SEMI, T_AND_IF, T_OR_IF, T_AMP, T_LESS, T_GREAT, T_DGREAT, T_GREAT2, T_GREAT2AND, T_END };

struct token {
    enum tok_type type;
    char *text;          /* T_WORD: expanded text */
};

/* --- variables --- */

static const char *var_get(const char *name)
{
    for (int i = 0; i < g_nvars; i++)
        if (strcmp(g_vars[i].name, name) == 0)
            return g_vars[i].value;
    return getenv(name);
}

static void var_set(const char *name, const char *value)
{
    if (getenv(name)) {
        setenv(name, value, 1);
        return;
    }
    for (int i = 0; i < g_nvars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            free(g_vars[i].value);
            g_vars[i].value = strdup(value);
            return;
        }
    }
    if (g_nvars < VARS_MAX) {
        g_vars[g_nvars].name = strdup(name);
        g_vars[g_nvars].value = strdup(value);
        g_nvars++;
    }
}

static void var_export(const char *name)
{
    for (int i = 0; i < g_nvars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            setenv(name, g_vars[i].value, 1);
            free(g_vars[i].name);
            free(g_vars[i].value);
            g_vars[i] = g_vars[--g_nvars];
            return;
        }
    }
    if (getenv(name) == NULL)
        setenv(name, "", 1);
}

/* Forget a shell variable without touching the environment. */
static void var_drop(const char *name)
{
    for (int i = 0; i < g_nvars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            free(g_vars[i].name);
            free(g_vars[i].value);
            g_vars[i] = g_vars[--g_nvars];
            return;
        }
    }
}

static void var_unset(const char *name)
{
    for (int i = 0; i < g_nvars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            free(g_vars[i].name);
            free(g_vars[i].value);
            g_vars[i] = g_vars[--g_nvars];
            break;
        }
    }
    unsetenv(name);
}

/* --- a growable string --- */

struct sbuf {
    char *s;
    size_t len, cap;
};

static int sbuf_put(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        while (ncap < b->len + n + 1)
            ncap *= 2;
        char *ns = realloc(b->s, ncap);
        if (ns == NULL)
            return -1;
        b->s = ns;
        b->cap = ncap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
    return 0;
}

static int sbuf_putc(struct sbuf *b, char c)
{
    return sbuf_put(b, &c, 1);
}

/* --- lexer with expansion --- */

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Expand one $-expression starting after the '$'; returns the bytes consumed. */
static size_t expand_dollar(const char *p, struct sbuf *out)
{
    char num[16];
    if (*p == '?') {
        snprintf(num, sizeof(num), "%d", g_last_status);
        sbuf_put(out, num, strlen(num));
        return 1;
    }
    if (*p == '$') {
        snprintf(num, sizeof(num), "%d", getpid());
        sbuf_put(out, num, strlen(num));
        return 1;
    }
    if (*p == '#') {
        snprintf(num, sizeof(num), "%d", g_script_argc);
        sbuf_put(out, num, strlen(num));
        return 1;
    }
    if (*p >= '0' && *p <= '9') {
        int i = *p - '0';
        const char *v = i == 0 ? g_script_name : (i <= g_script_argc ? g_script_args[i - 1] : "");
        sbuf_put(out, v, strlen(v));
        return 1;
    }
    char name[64];
    size_t n = 0;
    size_t consumed;
    if (*p == '{') {
        const char *q = p + 1;
        while (*q && *q != '}' && n < sizeof(name) - 1)
            name[n++] = *q++;
        if (*q != '}')
            return 0;   /* unterminated: leave the '$' literal */
        consumed = (size_t)(q - p) + 1;
    } else {
        const char *q = p;
        while (is_name_char(*q) && n < sizeof(name) - 1)
            name[n++] = *q++;
        if (n == 0)
            return 0;
        consumed = (size_t)(q - p);
    }
    name[n] = '\0';
    const char *v = var_get(name);
    if (v)
        sbuf_put(out, v, strlen(v));
    return consumed;
}

static int lex(const char *line, struct token *toks, int max, int *ntoks, int *had_assign_word)
{
    int n = 0;
    const char *p = line;
    (void)had_assign_word;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            break;
        if (n >= max - 1)
            return -1;
        struct token *t = &toks[n];
        t->text = NULL;
        if (*p == '|') {
            t->type = T_PIPE;
            p++;
            if (*p == '|') {
                t->type = T_OR_IF;
                p++;
            }
        } else if (*p == '&' && p[1] == '&') {
            t->type = T_AND_IF;
            p += 2;
        } else if (*p == '&') {
            t->type = T_AMP;   /* a bare &: run the pipeline in the background */
            p++;
        } else if (*p == ';') {
            t->type = T_SEMI;
            p++;
        } else if (*p == '<') {
            t->type = T_LESS;
            p++;
        } else if (*p == '>') {
            t->type = T_GREAT;
            p++;
            if (*p == '>') {
                t->type = T_DGREAT;
                p++;
            }
        } else if (*p == '2' && p[1] == '>') {
            /* "2>" and "2>&1" (a word never starts with "2>": the word lexer stops there) */
            p += 2;
            if (*p == '&' && p[1] == '1') {
                t->type = T_GREAT2AND;
                p += 2;
            } else {
                t->type = T_GREAT2;
            }
        } else {
            /* a word */
            struct sbuf b = { 0 };
            int quoted = 0;
            while (*p && !(quoted == 0 && (*p == ' ' || *p == '\t' || *p == '|' || *p == ';' || *p == '<' ||
                                           *p == '>' || *p == '&'))) {
                if (quoted == 0 && *p == '#' && b.len == 0)
                    break;
                if (quoted == 0 && *p == '2' && p[1] == '>' && b.len == 0)
                    break;
                if (*p == '\'' && quoted != 2) {
                    quoted = quoted == 1 ? 0 : 1;
                    p++;
                } else if (*p == '"' && quoted != 1) {
                    quoted = quoted == 2 ? 0 : 2;
                    p++;
                } else if (*p == '\\' && quoted != 1 && p[1]) {
                    if (quoted == 2 && !(p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        sbuf_putc(&b, *p);
                        p++;
                    } else {
                        sbuf_putc(&b, p[1]);
                        p += 2;
                    }
                } else if (*p == '$' && quoted != 1) {
                    size_t c = expand_dollar(p + 1, &b);
                    if (c == 0)
                        sbuf_putc(&b, *p++);
                    else
                        p += 1 + c;
                } else {
                    sbuf_putc(&b, *p++);
                }
            }
            if (quoted) {
                free(b.s);
                fprintf(stderr, "sh: unterminated quote\n");
                return -1;
            }
            t->type = T_WORD;
            t->text = b.s ? b.s : strdup("");
        }
        n++;
    }
    toks[n].type = T_END;
    toks[n].text = NULL;
    *ntoks = n;
    return 0;
}

/* --- parse --- */

struct redir {
    int fd;              /* 0, 1 or 2 */
    int flags;           /* open flags */
    char *path;          /* NULL for 2>&1 */
    int dup_to;          /* for 2>&1: 1 */
};

struct command {
    char *words[WORDS_MAX + 1];
    int nwords;
    struct redir redirs[REDIRS_MAX];
    int nredirs;
};

struct pipeline {
    struct command cmds[STAGES_MAX];
    int ncmds;
    int background;    /* ended with a bare `&` */
    char text[128];    /* what the user typed, for `jobs` */
};

static int parse_pipeline(struct token *toks, int *pos, struct pipeline *pl)
{
    memset(pl, 0, sizeof(*pl));
    for (;;) {
        if (pl->ncmds == STAGES_MAX)
            return -1;
        struct command *c = &pl->cmds[pl->ncmds];
        for (;;) {
            struct token *t = &toks[*pos];
            if (t->type == T_WORD) {
                if (c->nwords == WORDS_MAX)
                    return -1;
                c->words[c->nwords++] = t->text;
                (*pos)++;
            } else if (t->type == T_LESS || t->type == T_GREAT || t->type == T_DGREAT || t->type == T_GREAT2 ||
                       t->type == T_GREAT2AND) {
                if (c->nredirs == REDIRS_MAX)
                    return -1;
                struct redir *r = &c->redirs[c->nredirs];
                (*pos)++;
                if (t->type == T_GREAT2AND) {
                    r->fd = 2;
                    r->path = NULL;
                    r->dup_to = 1;
                } else {
                    if (toks[*pos].type != T_WORD)
                        return -1;
                    r->path = toks[*pos].text;
                    (*pos)++;
                    r->fd = t->type == T_LESS ? 0 : t->type == T_GREAT2 ? 2 : 1;
                    r->flags = t->type == T_LESS ? O_RDONLY
                               : t->type == T_DGREAT ? (O_WRONLY | O_CREAT | O_APPEND)
                                                     : (O_WRONLY | O_CREAT | O_TRUNC);
                }
                c->nredirs++;
            } else {
                break;
            }
        }
        c->words[c->nwords] = NULL;
        if (c->nwords == 0 && c->nredirs == 0)
            return -1;   /* empty command */
        pl->ncmds++;
        if (toks[*pos].type == T_PIPE) {
            (*pos)++;
            continue;
        }
        if (toks[*pos].type == T_AMP) {
            (*pos)++;
            pl->background = 1;
        }
        return 0;
    }
}

/* --- execution --- */

static int is_assignment(const char *w)
{
    const char *eq = strchr(w, '=');
    if (eq == NULL || eq == w)
        return 0;
    for (const char *p = w; p < eq; p++)
        if (!is_name_char(*p) || (p == w && *p >= '0' && *p <= '9'))
            return 0;
    return 1;
}

static int run_script_file(const char *path);
static void report_signal(const char *what, int status);
struct job;
static struct job *job_add(pid_t pgrp, const pid_t *pids, int npids, const char *text);
static struct job *job_pick(const char *spec);
static int job_wait_foreground(struct job *j);
static void jobs_poll(int announce);
static int job_all_done(const struct job *j);
static void job_print(const struct job *j, const char *state);
static void job_number(struct job *j);

static int builtin(struct command *c, int *is_builtin)
{
    const char *name = c->words[0];
    *is_builtin = 1;
    if (strcmp(name, "cd") == 0) {
        const char *dir = c->nwords > 1 ? c->words[1] : var_get("HOME");
        if (dir == NULL)
            dir = "/";
        if (chdir(dir) < 0) {
            fprintf(stderr, "sh: cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (strcmp(name, "pwd") == 0) {
        char buf[1024];
        if (getcwd(buf, sizeof(buf)) == NULL) {
            perror("sh: pwd");
            return 1;
        }
        printf("%s\n", buf);
        fflush(stdout);
        return 0;
    }
    if (strcmp(name, "exit") == 0) {
        int st = c->nwords > 1 ? atoi(c->words[1]) : g_last_status;
        fflush(stdout);
        exit(st & 0xff);
    }
    if (strcmp(name, "export") == 0) {
        for (int i = 1; i < c->nwords; i++) {
            char *eq = strchr(c->words[i], '=');
            if (eq) {
                *eq = '\0';
                setenv(c->words[i], eq + 1, 1);
                var_drop(c->words[i]);
                *eq = '=';
            } else {
                var_export(c->words[i]);
            }
        }
        return 0;
    }
    if (strcmp(name, "unset") == 0) {
        for (int i = 1; i < c->nwords; i++)
            var_unset(c->words[i]);
        return 0;
    }
    if (strcmp(name, "set") == 0) {
        if (c->nwords > 1 && strcmp(c->words[1], "-e") == 0) {
            g_opt_errexit = 1;
            return 0;
        }
        for (int i = 0; i < g_nvars; i++)
            printf("%s=%s\n", g_vars[i].name, g_vars[i].value);
        for (int i = 0; environ && environ[i]; i++)
            printf("%s\n", environ[i]);
        fflush(stdout);
        return 0;
    }
    if (strcmp(name, ":") == 0 || strcmp(name, "true") == 0)
        return 0;
    if (strcmp(name, "false") == 0)
        return 1;
    if (strcmp(name, "jobs") == 0) {
        jobs_poll(0);
        for (int i = 0; i < JOBS_MAX; i++) {
            struct job *j = &g_jobs[i];
            if (!j->used || j->id == 0)
                continue;
            if (job_all_done(j)) {
                job_print(j, j->last_status == 0 ? "Done" : "Exit");
                j->used = 0;
            } else {
                job_print(j, j->stopped ? "Stopped" : "Running");
                j->reported = 1;
            }
        }
        return 0;
    }
    if (strcmp(name, "fg") == 0 || strcmp(name, "bg") == 0) {
        int to_front = strcmp(name, "fg") == 0;
        if (!g_job_control) {
            fprintf(stderr, "sh: %s: no job control\n", name);
            return 1;
        }
        struct job *j = job_pick(c->nwords > 1 ? c->words[1] : NULL);
        if (j == NULL) {
            fprintf(stderr, "sh: %s: no such job\n", name);
            return 1;
        }
        printf("%s\n", j->text);
        fflush(stdout);
        if (to_front)
            (void)tcsetpgrp(0, j->pgrp);
        /* The SIGCONT goes to the group, so every stage of the pipeline
         * starts again together. */
        j->stopped = 0;
        j->reported = 0;
        for (int k = 0; k < j->npids; k++)
            j->parked[k] = 0;
        if (kill(-j->pgrp, SIGCONT) != 0 && errno != ESRCH)
            perror("sh: kill");
        g_current_job = j->id;
        if (!to_front)
            return 0;
        int st = job_wait_foreground(j);
        (void)tcsetpgrp(0, g_shell_pgrp);
        return st;
    }
    if (strcmp(name, "wait") == 0) {
        int st;
        while (waitpid(-1, &st, 0) > 0)
            ;
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "source") == 0) {
        if (c->nwords < 2) {
            fprintf(stderr, "sh: %s: filename argument required\n", name);
            return 2;
        }
        return run_script_file(c->words[1]);
    }
    *is_builtin = 0;
    return 0;
}

/* Apply a command's redirections to a handle map (child slots 0, 1, 2).
 * Returns the number of handles opened here (to close afterwards). */
static int apply_redirs(struct command *c, struct spawn_handle map[3], int opened[REDIRS_MAX])
{
    int n = 0;
    for (int i = 0; i < c->nredirs; i++) {
        struct redir *r = &c->redirs[i];
        if (r->path == NULL) {
            map[2].parent = map[1].parent;   /* 2>&1 */
            continue;
        }
        int fd = open(r->path, r->flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", r->path, strerror(errno));
            for (int k = 0; k < n; k++)
                close(opened[k]);
            return -1;
        }
        opened[n++] = fd;
        map[r->fd].parent = fd;
    }
    return n;
}

/* Run a builtin with its redirections applied to this process's own
 * handles, then restore them. */
static int run_builtin_redirected(struct command *c)
{
    int saved[3] = { -1, -1, -1 };
    struct spawn_handle map[3] = { { .child = 0, .parent = 0 }, { .child = 1, .parent = 1 },
                                   { .child = 2, .parent = 2 } };
    int opened[REDIRS_MAX];
    int nopen = apply_redirs(c, map, opened);
    if (nopen < 0)
        return 1;
    for (int fd = 0; fd < 3; fd++) {
        if (map[fd].parent != fd) {
            saved[fd] = dup(fd);
            dup2(map[fd].parent, fd);
        }
    }
    int is_b;
    int st = builtin(c, &is_b);
    fflush(stdout);
    for (int fd = 0; fd < 3; fd++) {
        if (saved[fd] >= 0) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        }
    }
    for (int k = 0; k < nopen; k++)
        close(opened[k]);
    return st;
}

static int run_pipeline(struct pipeline *pl)
{
    /* Assignments alone: shell variables. */
    if (pl->ncmds == 1 && pl->cmds[0].nredirs == 0) {
        struct command *c = &pl->cmds[0];
        int i = 0;
        while (i < c->nwords && is_assignment(c->words[i]))
            i++;
        if (i == c->nwords && i > 0) {
            for (int k = 0; k < i; k++) {
                char *eq = strchr(c->words[k], '=');
                *eq = '\0';
                var_set(c->words[k], eq + 1);
                *eq = '=';
            }
            return 0;
        }
        if (i > 0) {
            /* Leading assignments before a command: exported to the child only (simplified: set). */
            for (int k = 0; k < i; k++) {
                char *eq = strchr(c->words[k], '=');
                *eq = '\0';
                setenv(c->words[k], eq + 1, 1);
                *eq = '=';
            }
            memmove(c->words, c->words + i, (size_t)(c->nwords - i + 1) * sizeof(char *));
            c->nwords -= i;
        }
    }
    if (pl->ncmds == 1 && pl->cmds[0].nwords > 0) {
        int is_b;
        struct command *c = &pl->cmds[0];
        if (c->nredirs == 0) {
            int st = builtin(c, &is_b);
            if (is_b) {
                fflush(stdout);
                return st;
            }
        } else {
            /* Probe whether it is a builtin without running it. */
            static const char *const names[] = { "cd", "pwd", "exit", "export", "unset", "set", ":", "true",
                                                 "false", "wait", ".", "source",
                                                 "jobs",  "fg",   "bg" };
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
                if (strcmp(c->words[0], names[k]) == 0)
                    return run_builtin_redirected(c);
        }
    }

    fflush(stdout);
    /* What `jobs` prints: the words as they were parsed, which is close
     * enough to what was typed and does not need the raw line kept. */
    {
        size_t n = 0;
        pl->text[0] = '\0';
        for (int i = 0; i < pl->ncmds && n < sizeof(pl->text) - 1; i++) {
            for (int k = 0; k < pl->cmds[i].nwords && n < sizeof(pl->text) - 1; k++)
                n += (size_t)snprintf(pl->text + n, sizeof(pl->text) - n, "%s%s", n ? " " : "", pl->cmds[i].words[k]);
            if (i + 1 < pl->ncmds && n < sizeof(pl->text) - 1)
                n += (size_t)snprintf(pl->text + n, sizeof(pl->text) - n, " |");
        }
    }
    pid_t pids[STAGES_MAX];
    int prev_read = -1;
    int last_status = 0;
    /* Every stage of one pipeline is one job, so it is one process
     * group: ^C at the terminal interrupts the whole pipeline, not
     * whichever stage happened to be reading. The group is named by the
     * first stage's pid, which is why the first spawn asks for a group
     * of the child's own and the rest name it. */
    pid_t job_pgrp = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        struct command *c = &pl->cmds[i];
        struct spawn_handle map[3] = { { .child = 0, .parent = prev_read >= 0 ? prev_read : 0 },
                                       { .child = 1, .parent = 1 }, { .child = 2, .parent = 2 } };
        int pipefd[2] = { -1, -1 };
        if (i + 1 < pl->ncmds) {
            if (pipe(pipefd) < 0) {
                perror("sh: pipe");
                pids[i] = -1;
                break;
            }
            map[1].parent = pipefd[1];
        }
        int opened[REDIRS_MAX];
        int nopen = apply_redirs(c, map, opened);
        if (nopen < 0 || c->nwords == 0) {
            pids[i] = -1;
            last_status = 1;
        } else {
            pids[i] = g_job_control ? spawnvp_pgrp(c->words[0], (const char *const *)c->words, map, 3, job_pgrp)
                                    : spawnvp(c->words[0], (const char *const *)c->words, map, 3);
            if (pids[i] > 0 && job_pgrp == 0)
                job_pgrp = pids[i];
            if (pids[i] < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->words[0],
                        errno == ENOENT ? "not found" : errno == EACCES ? "not executable" : strerror(errno));
                last_status = errno == ENOENT ? 127 : 126;
            }
        }
        for (int k = 0; k < (nopen > 0 ? nopen : 0); k++)
            close(opened[k]);
        if (prev_read >= 0)
            close(prev_read);
        if (pipefd[1] >= 0)
            close(pipefd[1]);
        prev_read = pipefd[0];
    }
    if (prev_read >= 0)
        close(prev_read);

    /*
     * A background job is remembered and left alone: it keeps its own
     * process group, which is not the terminal's foreground one, so it
     * cannot read the line the shell is waiting for (the kernel stops it
     * with SIGTTIN if it tries).
     */
    if (pl->background && g_job_control && job_pgrp != 0) {
        struct job *j = job_add(job_pgrp, pids, pl->ncmds, pl->text);
        if (j != NULL) {
            job_number(j);
            printf("[%d] %d\n", j->id, (int)job_pgrp);
        }
        return 0;
    }
    if (pl->background) {
        /* No job control: nothing can be handed a terminal or continued,
         * so the honest thing is to run it in the foreground rather than
         * pretend. Scripts reach this. */
        fprintf(stderr, "sh: no job control: running in the foreground\n");
    }

    /* Hand the terminal to the job while it runs, and take it back
     * afterwards. Both may fail -- a job that has already exited is no
     * longer a group -- and neither failure changes what happens next.
     * Taking it back is done from the background, which is a SIGTTOU;
     * the shell ignores that signal, which is what makes it legal. */
    if (g_job_control && job_pgrp != 0)
        (void)tcsetpgrp(0, job_pgrp);
    if (g_job_control && job_pgrp != 0) {
        struct job *j = job_add(job_pgrp, pids, pl->ncmds, pl->text);
        if (j != NULL) {
            last_status = job_wait_foreground(j);
        } else {
            for (int i = 0; i < pl->ncmds; i++) {
                if (pids[i] < 0)
                    continue;
                int st = 0;
                if (waitpid(pids[i], &st, 0) == pids[i] && i == pl->ncmds - 1)
                    last_status = st;
            }
        }
    } else {
        for (int i = 0; i < pl->ncmds; i++) {
            if (pids[i] < 0)
                continue;
            int st = 0;
            if (waitpid(pids[i], &st, 0) == pids[i] && i == pl->ncmds - 1)
                last_status = st;
        }
    }
    if (g_job_control)
        (void)tcsetpgrp(0, g_shell_pgrp);
    if (last_status != JOB_STOPPED && last_status > 128 && last_status < 256 &&
        pl->cmds[pl->ncmds - 1].nwords > 0)
        report_signal(pl->cmds[pl->ncmds - 1].words[0], last_status);
    return last_status;
}

static int run_line(const char *line)
{
    struct token toks[TOKENS_MAX];
    int ntoks = 0, dummy = 0;
    if (lex(line, toks, TOKENS_MAX, &ntoks, &dummy) < 0) {
        g_last_status = 2;
        return -1;
    }
    int pos = 0;
    int rc = 0;
    int skip = 0;   /* 0: run, 1: skip until next ';', 2: skip because && failed / || succeeded */
    while (toks[pos].type != T_END) {
        struct pipeline pl;
        if (toks[pos].type == T_SEMI || toks[pos].type == T_AMP) {
            pos++;
            skip = 0;
            continue;
        }
        if (parse_pipeline(toks, &pos, &pl) < 0) {
            fprintf(stderr, "sh: syntax error near '%s'\n", toks[pos].text ? toks[pos].text : "");
            g_last_status = 2;
            rc = -1;
            break;
        }
        if (!skip) {
            g_last_status = run_pipeline(&pl);
            if (g_opt_errexit && g_last_status != 0 && toks[pos].type != T_AND_IF && toks[pos].type != T_OR_IF) {
                fflush(stdout);
                exit(g_last_status);
            }
        }
        if (toks[pos].type == T_AND_IF) {
            pos++;
            if (!skip)
                skip = g_last_status != 0 ? 2 : 0;
        } else if (toks[pos].type == T_OR_IF) {
            pos++;
            if (!skip)
                skip = g_last_status == 0 ? 2 : 0;
        } else if (toks[pos].type == T_SEMI) {
            pos++;
            skip = 0;
        } else if (toks[pos].type != T_END) {
            fprintf(stderr, "sh: syntax error near '%s'\n", toks[pos].text ? toks[pos].text : "");
            g_last_status = 2;
            rc = -1;
            break;
        }
    }
    for (int i = 0; i < ntoks; i++)
        free(toks[i].text);
    return rc;
}

static int run_script_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 127;
    }
    char line[LINE_MAX_];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[n - 1] = '\0';
        run_line(line);
    }
    fclose(f);
    return g_last_status;
}

/*
 * Take charge of the terminal, if there is one to take. A shell that is
 * not already a process group leader starts a session, which is what
 * gives it a terminal of its own; then it claims the terminal for its
 * own group. Both can fail -- a shell started from another shell's job,
 * a terminal another session already holds -- and a failure simply
 * means no job control: commands still run, they just share the
 * shell's group and its signals.
 */
static void job_control_init(void)
{
    if (getpgrp() != getpid() && setsid() < 0)
        return;
    g_shell_pgrp = getpgrp();
    /* The shell survives what it sends to its jobs: it is about to make
     * each job the foreground group, and it must still be here to print
     * a prompt when the job dies of the interrupt. Set before the
     * terminal is claimed, not after, so there is no instant in which
     * the shell is the foreground group and still dies of a ^C. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    /* And the two the terminal sends for touching it from the
     * background. The shell hands the terminal to each job and takes it
     * back afterwards, and taking it back is by definition done from
     * the background -- so a shell that did not ignore SIGTTOU would
     * stop itself every time a job finished. SIGTSTP likewise: ^Z is for
     * the job, not for the shell that is waiting on it. */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    if (tcsetpgrp(0, g_shell_pgrp) != 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        return;
    }
    g_job_control = 1;
}

/* --- the job table ---------------------------------------------------------- */

static struct job *job_find(int id)
{
    if (id == 0)
        return NULL;
    for (int i = 0; i < JOBS_MAX; i++)
        if (g_jobs[i].used && g_jobs[i].id == id)
            return &g_jobs[i];
    return NULL;
}

static struct job *job_slot(void)
{
    for (int i = 0; i < JOBS_MAX; i++)
        if (!g_jobs[i].used)
            return &g_jobs[i];
    return NULL;
}

/* A job gets its number when it outlives the foreground -- when it is
 * put in the background, or when it stops. Numbering every pipeline
 * would have `[9]+ Stopped` on the ninth command of the session, which
 * is not what a job number means. */
static void job_number(struct job *j)
{
    if (j->id == 0) {
        j->id = g_next_job_id++;
        g_current_job = j->id;
    }
}

/* `%n`, `%%`/`%+`, or nothing: the job a `fg`/`bg` argument names. */
static struct job *job_pick(const char *spec)
{
    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
        struct job *j = job_find(g_current_job);
        if (j != NULL && !job_all_done(j))
            return j;
        for (int i = 0; i < JOBS_MAX; i++)
            if (g_jobs[i].used && g_jobs[i].id != 0 && !job_all_done(&g_jobs[i]))
                return &g_jobs[i];
        return NULL;
    }
    if (spec[0] == '%')
        spec++;
    struct job *j = job_find(atoi(spec));
    return (j != NULL && !job_all_done(j)) ? j : NULL;
}

static struct job *job_add(pid_t pgrp, const pid_t *pids, int npids, const char *text)
{
    struct job *j = job_slot();
    if (j == NULL)
        return NULL;   /* the table is full: the job still runs, it is just not tracked */
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->pgrp = pgrp;
    j->npids = npids;
    for (int i = 0; i < npids; i++)
        j->pids[i] = pids[i];
    snprintf(j->text, sizeof(j->text), "%s", text);
    return j;
}

static int job_all_done(const struct job *j)
{
    for (int i = 0; i < j->npids; i++)
        if (j->pids[i] > 0 && !j->done[i])
            return 0;
    return 1;
}

/* Fold one waitpid result into whichever job owns the pid. Returns the
 * job, or NULL when the pid belongs to none of them. */
static struct job *job_note(pid_t pid, int status)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        struct job *j = &g_jobs[i];
        if (!j->used)
            continue;
        for (int k = 0; k < j->npids; k++) {
            if (j->pids[k] != pid)
                continue;
            if (WIFSTOPPED(status)) {
                j->stopped = 1;
                j->reported = 0;
                job_number(j);   /* it has outlived the foreground */
            } else if (WIFCONTINUED(status)) {
                j->stopped = 0;
                j->reported = 0;
                for (int m = 0; m < j->npids; m++)
                    j->parked[m] = 0;
            } else {
                j->done[k] = 1;
                j->stopped = 0;
                if (k == j->npids - 1)
                    j->last_status = status;
            }
            return j;
        }
    }
    return NULL;
}

static void job_print(const struct job *j, const char *state)
{
    printf("[%d]%s  %-24s %s\n", j->id, j->id == g_current_job ? "+" : " ", state, j->text);
}

/* Collect what happened to background jobs without blocking, and say so.
 * Called before each prompt, which is where a shell reports these. */
static void jobs_poll(int announce)
{
    for (;;) {
        int st = 0;
        pid_t pid = waitpid(-1, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0)
            break;
        struct job *j = job_note(pid, st);
        if (j == NULL || !announce)
            continue;
        if (j->stopped && !j->reported) {
            job_print(j, "Stopped");
            j->reported = 1;
        } else if (job_all_done(j) && j->id != 0) {
            job_print(j, j->last_status == 0 ? "Done" : "Exit");
            j->used = 0;
        } else if (job_all_done(j)) {
            j->used = 0;   /* a foreground job nobody was told about */
        }
    }
}

/*
 * Wait for one job in the foreground: the terminal is already its.
 *
 * A ^Z stops the whole group, so every live stage has a stop to report
 * and this waits for all of them before returning. Returning on the
 * first would hand the terminal back to the shell while the other
 * stages were still on their way to parking, and they would write over
 * the prompt.
 */
static int job_wait_foreground(struct job *j)
{
    int status = 0;
    int any_stopped = 0;
    for (;;) {
        int alive = 0;
        for (int i = 0; i < j->npids; i++) {
            if (j->pids[i] <= 0 || j->done[i] || j->parked[i])
                continue;
            alive = 1;
            int st = 0;
            pid_t got = waitpid(j->pids[i], &st, WUNTRACED);
            if (got != j->pids[i])
                break;
            if (WIFSTOPPED(st)) {
                j->parked[i] = 1;
                any_stopped = 1;
                continue;   /* the rest of the group is stopping too */
            }
            j->done[i] = 1;
            if (i == j->npids - 1)
                status = st;
        }
        if (!alive)
            break;
    }
    if (any_stopped) {
        j->stopped = 1;
        j->reported = 1;
        job_number(j);
        job_print(j, "Stopped");
        return JOB_STOPPED;
    }
    j->last_status = status;
    j->used = 0;   /* finished in the foreground: nothing to remember */
    return status;
}

/* What to say about a job that did not exit on its own. The terminal has
 * already echoed "^C", so an interrupt needs no announcement; anything
 * else does, or the shell would report a plausible-looking exit status
 * for a command that was killed. */
static void report_signal(const char *what, int status)
{
    static const char *const names[] = { [SIGHUP] = "hangup",  [SIGINT] = "interrupt", [SIGQUIT] = "quit",
                                         [SIGILL] = "illegal instruction", [SIGABRT] = "aborted",
                                         [SIGBUS] = "bus error", [SIGFPE] = "arithmetic exception",
                                         [SIGKILL] = "killed", [SIGSEGV] = "segmentation fault",
                                         [SIGPIPE] = "broken pipe", [SIGTERM] = "terminated" };
    int sig = status - 128;
    if (sig <= 0 || sig >= NSIG || sig == SIGINT || sig == SIGPIPE)
        return;
    const char *name = (size_t)sig < sizeof(names) / sizeof(names[0]) && names[sig] ? names[sig] : NULL;
    if (name)
        fprintf(stderr, "sh: %s: %s\n", what, name);
    else
        fprintf(stderr, "sh: %s: signal %d\n", what, sig);
}

static void interactive(void)
{
    char line[LINE_MAX_];
    for (;;) {
        jobs_poll(1);   /* what background jobs did while we were away */
        fflush(stdout);
        write(2, "cosmo$ ", 7);
        ssize_t n = read(0, line, sizeof(line) - 1);
        if (n < 0) {
            perror("sh: read");
            exit(1);
        }
        if (n == 0) {
            write(2, "\n", 1);   /* ^D */
            exit(g_last_status);
        }
        line[n] = '\0';
        if (n && line[n - 1] == '\n')
            line[n - 1] = '\0';
        run_line(line);
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            g_opt_errexit = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "sh: -c needs a command\n");
                return 2;
            }
            g_script_name = "sh";
            g_script_args = argv + i + 2;
            g_script_argc = argc - i - 2;
            run_line(argv[i + 1]);
            fflush(stdout);
            return g_last_status;
        } else {
            fprintf(stderr, "sh: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i < argc) {
        g_script_name = argv[i];
        g_script_args = argv + i + 1;
        g_script_argc = argc - i - 1;
        int st = run_script_file(argv[i]);
        fflush(stdout);
        return st;
    }
    g_interactive = 1;
    job_control_init();
    interactive();
    return 0;
}
