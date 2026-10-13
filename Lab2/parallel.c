#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <pthread.h>

typedef struct {
    int *a, *tmp;
    size_t left, right, budget;
    int demo;
} ThreadArgs;

static void thread_error(int code, const char *operation)
{
    if (code) {
        fprintf(stderr, "%s: %s\n", operation, strerror(code));
        /* Завершается весь процесс: частичный результат не выдаётся. */
        exit(EXIT_FAILURE);
    }
}

static void *work(void *pointer)
{
    ThreadArgs *args = pointer;
    size_t length = args->right - args->left;
    if (args->budget <= 1 || length < 2) {
        /* Только для наблюдения потоков через ps, не для экспериментов. */
        if (args->demo) {
            struct timespec delay = {10, 0};
            while (nanosleep(&delay, &delay) && errno == EINTR) {}
        }
        merge_sort(args->a, args->tmp, args->left, args->right);
        return NULL;
    }
    size_t mid = args->left + length / 2;
    size_t left_budget = args->budget / 2;
    ThreadArgs left = {args->a, args->tmp, args->left, mid,
                       left_budget, args->demo};
    ThreadArgs right = {args->a, args->tmp, mid, args->right,
                        args->budget - left_budget, args->demo};
    pthread_t thread;
    thread_error(pthread_create(&thread, NULL, work, &left), "pthread_create");
    work(&right); /* Текущий поток тоже выполняет сортировку. */
    thread_error(pthread_join(thread, NULL), "pthread_join");
    merge(args->a, args->tmp, args->left, mid, args->right);
    return NULL;
}

int main(int argc, char **argv)
{
    Options o = options(argc, argv, 1);
    int *a = input(&o);
    int *tmp = allocate(o.n);
    /* Нельзя занять больше потоков, чем элементов массива. */
    if (o.threads > o.n) o.threads = o.n ? o.n : 1;
    if (o.demo) fprintf(stderr, "PID=%ld; demonstration delay is included in time\n", (long)getpid());
    ThreadArgs args = {a, tmp, 0, o.n, o.threads, o.demo};
    double start = now();
    work(&args);
    double elapsed = now() - start;
    output(a, &o, elapsed);
    free(tmp);
    free(a);
    return EXIT_SUCCESS;
}
