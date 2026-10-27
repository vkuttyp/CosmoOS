/* The library tests/tcc/dl-main.c opens (selfhost.sh's programs). */
int greet_answer = 42;

const char *greet(void)
{
    return "loaded by dlopen";
}
