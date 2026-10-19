/* libdltest.c - the library dltest opens (roadmap 1.1 P4). The shipped
 * one, at /boot/tests/dyn/lib/libdltest.so, answers 42; built with
 * -DSHADOW it answers 7 (the copy LD_LIBRARY_PATH names). */
#ifndef ANSWER
#define ANSWER 42
#endif
int dltest_counter = 1;

int dltest_answer(void)
{
    dltest_counter++;
    return ANSWER;
}
