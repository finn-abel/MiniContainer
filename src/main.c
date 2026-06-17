#include <stdio.h>
#include <string.h>

static void print_usage(FILE *stream)
{
    fprintf(stream, "Usage: minictl <command> [options]\n");
}

int main(int argc, char **argv)
{
    /*
     * The baseline CLI only exposes usage until the parser module exists.
     * Unknown commands fail so future tests can rely on non-zero errors.
     */
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return 0;
    }

    print_usage(stderr);
    return 1;
}
