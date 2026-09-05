/* version.c - Versions, dependency constraints, names (docs/pkg/design.md). */

#include <ctype.h>
#include <string.h>

#include "pkg.h"

bool name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PKG_NAME_MAX)
        return false;
    if (!(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0])))
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '+' || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool version_parse(const char *s, struct version *v)
{
    memset(v, 0, sizeof(*v));
    if (*s == '\0' || strlen(s) >= PKG_VERSION_MAX)
        return false;
    const char *p = s;
    for (;;) {
        if (!isdigit((unsigned char)*p) || v->ncomp == 8)
            return false;
        unsigned n = 0;
        while (isdigit((unsigned char)*p)) {
            if (n > 100000000u)
                return false;
            n = n * 10 + (unsigned)(*p++ - '0');
        }
        v->comp[v->ncomp++] = n;
        if (*p == '.') {
            p++;
            continue;
        }
        break;
    }
    if (*p == '-') {
        p++;
        if (!isdigit((unsigned char)*p))
            return false;
        unsigned n = 0;
        while (isdigit((unsigned char)*p)) {
            if (n > 100000000u)
                return false;
            n = n * 10 + (unsigned)(*p++ - '0');
        }
        v->rev = n;
    }
    return *p == '\0';
}

int version_cmp(const char *a, const char *b)
{
    struct version va, vb;
    if (!version_parse(a, &va) || !version_parse(b, &vb))
        return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0;
    int n = va.ncomp > vb.ncomp ? va.ncomp : vb.ncomp;
    for (int i = 0; i < n; i++) {
        unsigned x = i < va.ncomp ? va.comp[i] : 0;
        unsigned y = i < vb.ncomp ? vb.comp[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (va.rev != vb.rev)
        return va.rev < vb.rev ? -1 : 1;
    return 0;
}

const char *op_text(enum cmp_op op)
{
    switch (op) {
    case OP_EQ: return "=";
    case OP_GE: return ">=";
    case OP_LE: return "<=";
    case OP_LT: return "<";
    case OP_GT: return ">";
    default: return "";
    }
}

bool depend_parse(const char *text, struct depend *d)
{
    memset(d, 0, sizeof(*d));
    const char *p = text;
    while (*p == ' ')
        p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '=' && *p != '<' && *p != '>' && n < PKG_NAME_MAX - 1)
        d->name[n++] = *p++;
    d->name[n] = '\0';
    if (!name_valid(d->name))
        return false;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return true;
    if (p[0] == '>' && p[1] == '=') {
        d->op = OP_GE;
        p += 2;
    } else if (p[0] == '<' && p[1] == '=') {
        d->op = OP_LE;
        p += 2;
    } else if (*p == '=') {
        d->op = OP_EQ;
        p++;
    } else if (*p == '<') {
        d->op = OP_LT;
        p++;
    } else if (*p == '>') {
        d->op = OP_GT;
        p++;
    } else {
        return false;
    }
    while (*p == ' ')
        p++;
    n = 0;
    while (*p && *p != ' ' && n < PKG_VERSION_MAX - 1)
        d->version[n++] = *p++;
    d->version[n] = '\0';
    while (*p == ' ')
        p++;
    struct version v;
    return *p == '\0' && version_parse(d->version, &v);
}

bool depend_satisfied(const struct depend *d, const char *version)
{
    int c = version_cmp(version, d->version);
    switch (d->op) {
    case OP_NONE: return true;
    case OP_EQ: return c == 0;
    case OP_GE: return c >= 0;
    case OP_LE: return c <= 0;
    case OP_LT: return c < 0;
    case OP_GT: return c > 0;
    }
    return false;
}
