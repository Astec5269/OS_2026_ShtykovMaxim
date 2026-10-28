#ifndef LAB_IPC_H
#define LAB_IPC_H
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <time.h>
#include <signal.h>

#define DATA_SIZE 4096
#define END_DATA UINT32_MAX
/* status: 0 — успешно; 1 — ошибка обработки. */
typedef struct {
    uint32_t length;
    int status;
    char data[DATA_SIZE];
} Shared;

/* stdio.h не используется. */
static inline size_t text_length(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

/* write может записать только часть запрошенных байтов. */
static inline int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
            if (k == 0) errno = EIO;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static inline int put_text(int fd, const char *s) {
    return write_all(fd, s, text_length(s));
}

/* 1: получен символ, 0: EOF, -1: ошибка. */
static inline int read_byte(int fd, char *c) {
    ssize_t n;
    do { n = read(fd, c, 1); } while (n < 0 && errno == EINTR);
    return (int)n;
}

/* Перевод числа в десятичную строку, в том числе LLONG_MIN. */
static inline int put_number(int fd, long long value) {
    char buffer[32];
    size_t pos = sizeof(buffer);
    unsigned long long magnitude = value < 0
        ? 0ULL - (unsigned long long)value : (unsigned long long)value;
    do {
        buffer[--pos] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) buffer[--pos] = '-';
    return write_all(fd, buffer + pos, sizeof(buffer) - pos);
}

static inline void report_error(const char *operation) {
    int saved = errno;
    /* Если сломан сам stderr, сообщить об этом через него уже невозможно. */
    (void)put_text(STDERR_FILENO, operation);
    (void)put_text(STDERR_FILENO, ": errno=");
    (void)put_number(STDERR_FILENO, saved);
    (void)put_text(STDERR_FILENO, "\n");
}

/* В Linux close после EINTR не повторяем: дескриптор уже освобождён. */
static inline int close_fd(int *fd) {
    if (*fd < 0) return 0;
    int old = *fd;
    *fd = -1;
    if (close(old) < 0) {
        report_error("close");
        return -1;
    }
    return 0;
}


/* Ожидание с периодической проверкой: родитель ещё существует?
   kill(pid, 0) не посылает сигнал. Таймаут не ограничивает время ввода. */
static inline int wait_peer(sem_t *sem, pid_t peer) {
    for (;;) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) < 0) return -1;
        ++deadline.tv_sec;
        if (sem_timedwait(sem, &deadline) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) return -1;
        if (getppid() != peer) { errno = ECHILD; return -1; }
        if (kill(peer, 0) < 0 && errno == ESRCH) return -1;
    }
}
#endif
