/*
 * test_upgrade_be.c - the boot-environment pairing file \cosmo\be
 * (userland/system/upgrade_be.c; roadmap 1.2 B4), on the host.
 */

#include "harness.h"

#include "../../userland/system/upgrade_be.h"

#include <stdio.h>
#include <string.h>

static void hexrun(char *out, size_t n, char c)
{
    memset(out, c, n);
    out[n] = '\0';
}

static void test_roundtrip(void)
{
    struct be_file b, c;
    memset(&b, 0, sizeof(b));
    char set_a[BE_SET_HEX + 1], set_b[BE_SET_HEX + 1], u1[BE_UUID_HEX + 1], u2[BE_UUID_HEX + 1];
    hexrun(set_a, BE_SET_HEX, 'a');
    hexrun(set_b, BE_SET_HEX, 'b');
    hexrun(u1, BE_UUID_HEX, '1');
    hexrun(u2, BE_UUID_HEX, '2');
    snprintf(b.intent, sizeof(b.intent), "%s", set_b);
    be_add(&b, "be-v1.0.8", u1, set_a);
    be_add(&b, "be-v1.0.9", u2, set_b);
    char text[4096], err[120];
    size_t len = be_format(&b, text, sizeof(text));
    EXPECT(len > 0 && strncmp(text, "cosmo-be 1\nintent ", 18) == 0);
    EXPECT(be_parse(text, len, &c, err, sizeof(err)) == 0);
    EXPECT(c.n == 2 && strcmp(c.intent, set_b) == 0);
    EXPECT(strcmp(c.env[0].name, "be-v1.0.8") == 0 && strcmp(c.env[0].uuid, u1) == 0 && strcmp(c.env[0].set, set_a) == 0);
    EXPECT(be_for_set(&c, set_a) == &c.env[0] && be_for_set(&c, set_b) == &c.env[1]);
    char set_c[BE_SET_HEX + 1];
    hexrun(set_c, BE_SET_HEX, 'c');
    EXPECT(be_for_set(&c, set_c) == NULL);
    /* No intent line is valid too: an ESP that only ever had environments. */
    c.intent[0] = '\0';
    len = be_format(&c, text, sizeof(text));
    EXPECT(be_parse(text, len, &b, err, sizeof(err)) == 0 && b.intent[0] == '\0' && b.n == 2);
    /* Too small a buffer formats nothing rather than a cut file. */
    EXPECT(be_format(&c, text, 40) == 0);
}

static void test_refused(void)
{
    struct be_file b;
    char err[120];
    char set[BE_SET_HEX + 1], u[BE_UUID_HEX + 1], line[512];
    hexrun(set, BE_SET_HEX, 'a');
    hexrun(u, BE_UUID_HEX, '1');
    static const char *const bad[] = {
        "",                                   /* empty */
        "cosmo-be 2\n",                       /* another version */
        "intent aa\n",                        /* no header */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        EXPECT(be_parse(bad[i], strlen(bad[i]), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nenv -bad %s %s\n", u, set);       /* a name cosmo-snap refuses */
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nenv a/b %s %s\n", u, set);
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nenv be-x %.31s %s\n", u, set);    /* a short uuid */
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nenv be-x %s %sG\n", u, set);      /* not hex, not 128 */
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nintent %.127sA\n", set);          /* upper case: not this file */
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
    snprintf(line, sizeof(line), "cosmo-be 1\nenv be-x %s %s extra\n", u, set); /* a fifth field */
    EXPECT(be_parse(line, strlen(line), &b, err, sizeof(err)) < 0);
}

static void test_capacity(void)
{
    struct be_file b;
    memset(&b, 0, sizeof(b));
    char set[BE_SET_HEX + 1], u[BE_UUID_HEX + 1], name[32];
    hexrun(set, BE_SET_HEX, 'a');
    hexrun(u, BE_UUID_HEX, '1');
    for (int i = 0; i < BE_MAX + 3; i++) {
        snprintf(name, sizeof(name), "be-%d", i);
        be_add(&b, name, u, set);
    }
    EXPECT(b.n == BE_MAX && strcmp(b.env[0].name, "be-3") == 0);   /* the oldest went */
    be_drop(&b, 0);
    EXPECT(b.n == BE_MAX - 1 && strcmp(b.env[0].name, "be-4") == 0);
    be_drop(&b, 99);   /* out of range: nothing */
    EXPECT(b.n == BE_MAX - 1);
}

static bool taken_two(const char *name, void *arg)
{
    (void)arg;
    return strcmp(name, "be-v1.0.8") == 0 || strcmp(name, "be-v1.0.8-2") == 0;
}

static void test_name(void)
{
    char out[BE_NAME_MAX + 1];
    be_name("v1.0.8", NULL, NULL, out);
    EXPECT(strcmp(out, "be-v1.0.8") == 0);
    be_name("v1.0.8", taken_two, NULL, out);
    EXPECT(strcmp(out, "be-v1.0.8-3") == 0);
    be_name("v1.0.7-123-g50ef1f4d-dirty-and-much-longer", NULL, NULL, out);
    EXPECT(strlen(out) <= BE_NAME_MAX - 3 && strncmp(out, "be-v1.0.7-123", 13) == 0);   /* room for a suffix */
    be_name("a/b c", NULL, NULL, out);
    EXPECT(strcmp(out, "be-a_b_c") == 0);
    be_name("", NULL, NULL, out);
    EXPECT(strcmp(out, "be-unknown") == 0);
}

static const struct host_test tests[] = {
    { "format and parse back", test_roundtrip },
    { "malformed files refused", test_refused },
    { "at most BE_MAX, the oldest dropped", test_capacity },
    { "environment names", test_name },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
