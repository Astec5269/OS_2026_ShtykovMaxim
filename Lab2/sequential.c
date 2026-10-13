#define _POSIX_C_SOURCE 200809L
#include "common.h"

int main(int argc, char **argv)
{
    Options o = options(argc, argv, 0);
    int *a = input(&o);
    int *tmp = allocate(o.n);
    double start = now();
    merge_sort(a, tmp, 0, o.n);
    double elapsed = now() - start;
    output(a, &o, elapsed);
    free(tmp);
    free(a);
    return EXIT_SUCCESS;
}
