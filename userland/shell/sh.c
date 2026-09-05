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
 */

#include <errno.h>
#include <fcntl.h>
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

enum tok_type { T_WORD, T_PIPE, T_SEMI, T_AND_IF, T_OR_IF, T_LESS, T_GREAT, T_DGREAT, T_GREAT2, T_GREAT2AND, T_END };

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
                                           *p == '>' || (*p == '&' && p[1] == '&')))) {
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
    struct spawn_handle map[3] = { { 0, 0 }, { 1, 1 }, { 2, 2 } };
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
                                                 "false", "wait", ".", "source" };
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
                if (strcmp(c->words[0], names[k]) == 0)
                    return run_builtin_redirected(c);
        }
    }

    fflush(stdout);
    pid_t pids[STAGES_MAX];
    int prev_read = -1;
    int last_status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        struct command *c = &pl->cmds[i];
        struct spawn_handle map[3] = { { 0, prev_read >= 0 ? prev_read : 0 }, { 1, 1 }, { 2, 2 } };
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
            pids[i] = spawnvp(c->words[0], (const char *const *)c->words, map, 3);
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
    for (int i = 0; i < pl->ncmds; i++) {
        if (pids[i] < 0)
            continue;
        int st = 0;
        if (waitpid(pids[i], &st, 0) == pids[i] && i == pl->ncmds - 1)
            last_status = st;
    }
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
        if (toks[pos].type == T_SEMI) {
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

static void interactive(void)
{
    char line[LINE_MAX_];
    for (;;) {
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
    interactive();
    return 0;
}
