#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <limits.h>

/* Разбор идёт по символам: длина строки и число чисел не фиксированы. */
typedef struct {
    unsigned long long magnitude;
    long long sum;
    int in_token, negative, digits, have_number, invalid;
} Parser;

static void finish_number(Parser *p) {
    if (!p->in_token) return;
    if (!p->digits) p->invalid = 1;
    if (!p->invalid) {
        int number;
        if (p->negative && p->magnitude == (unsigned long long)INT_MAX + 1ULL)
            number = INT_MIN;
        else number = p->negative ? -(int)p->magnitude : (int)p->magnitude;
        if ((number > 0 && p->sum > LLONG_MAX - number) ||
            (number < 0 && p->sum < LLONG_MIN - number)) p->invalid = 1;
        else { p->sum += number; p->have_number = 1; }
    }
    p->magnitude = 0;
    p->in_token = p->negative = p->digits = 0;
}

static void consume(Parser *p, char c) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
        finish_number(p);
        return;
    }
    if (!p->in_token) {
        p->in_token = 1;
        if (c == '-' || c == '+') { p->negative = c == '-'; return; }
    }
    if (c < '0' || c > '9') { p->invalid = 1; return; }
    p->digits = 1;
    unsigned digit = (unsigned)(c - '0');
    unsigned long long limit = (unsigned long long)INT_MAX + (p->negative ? 1ULL : 0ULL);
    if (p->magnitude > (limit - digit) / 10ULL) p->invalid = 1;
    else p->magnitude = p->magnitude * 10ULL + digit;
}

/* Одна законченная строка — одна сумма в файле. */
static int save_sum(int file, Parser *p) {
    finish_number(p);
    if (p->invalid || !p->have_number) {
        (void)put_text(2, "Invalid integer, empty command or sum overflow.\n");
        return -1;
    }
    if (put_number(file, p->sum) < 0 || put_text(file, "\n") < 0) {
        report_error("write output file"); return -1;
    }
    *p = (Parser){0};
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        (void)put_text(2, "Usage: client filename shm_name request_sem reply_sem\n");
        return 1;
    }
    int result = 1, file = -1, shm = -1;
    Shared *shared = MAP_FAILED;
    sem_t *request = SEM_FAILED, *reply = SEM_FAILED;
    pid_t parent = getppid();
    if (parent == 1) return 1;

    shm = shm_open(argv[2], O_RDWR, 0);
    if (shm < 0) { report_error("shm_open client"); goto cleanup; }
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED, shm, 0);
    if (shared == MAP_FAILED) { report_error("mmap client"); goto cleanup; }
    request = sem_open(argv[3], 0);
    if (request == SEM_FAILED) { report_error("sem_open request"); goto cleanup; }
    reply = sem_open(argv[4], 0);
    if (reply == SEM_FAILED) { report_error("sem_open reply"); goto cleanup; }

    file = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    shared->status = file < 0;
    if (file < 0) report_error("open output file");
    /* Стартовое подтверждение: IPC подключено, файл открыт или ошибка. */
    if (sem_post(reply) < 0) { report_error("sem_post ready"); goto cleanup; }
    if (file < 0) goto cleanup;

    Parser parser = {0};
    for (;;) {
        if (wait_peer(request, parent) < 0) {
            report_error("wait request"); goto cleanup;
        }
        int end = shared->length == END_DATA;
        int failed = 0;
        if (end) {
            if (parser.in_token || parser.have_number || parser.invalid)
                failed = save_sum(file, &parser) < 0;
            if (close_fd(&file) < 0) failed = 1;
        } else if (shared->length > DATA_SIZE) {
            (void)put_text(2, "Invalid shared block length.\n");
            failed = 1;
        } else {
            for (uint32_t i = 0; i < shared->length; ++i) {
                if (shared->data[i] == '\n') {
                    if (save_sum(file, &parser) < 0) { failed = 1; break; }
                } else consume(&parser, shared->data[i]);
            }
        }
        /* Данные остаются в общей памяти и доступны родителю как эхо. */
        shared->status = failed;
        if (sem_post(reply) < 0) { report_error("sem_post reply"); goto cleanup; }
        if (failed) goto cleanup;
        if (end) { result = 0; break; }
    }
cleanup:
    if (close_fd(&file) < 0) result = 1;
    if (request != SEM_FAILED && sem_close(request) < 0) {
        report_error("sem_close request"); result = 1;
    }
    if (reply != SEM_FAILED && sem_close(reply) < 0) {
        report_error("sem_close reply"); result = 1;
    }
    if (shared != MAP_FAILED && munmap(shared, sizeof(*shared)) < 0) {
        report_error("munmap client"); result = 1;
    }
    if (close_fd(&shm) < 0) result = 1;
    return result;
}
