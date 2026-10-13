#include <fcntl.h>
#include <limits.h>

#include <unistd.h>
#include <errno.h>
#include <stddef.h>

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
    if (argc != 2) {
        (void)put_text(2, "Usage: child filename\n"); return 1;
    }
    int file = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
    if (file < 0) { report_error("open output file"); return 1; }

    char buf[4096];
    Parser parser = {0};
    int result = 1;
    for (;;) {
        ssize_t bytes = read(STDIN_FILENO, buf, sizeof(buf));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) { report_error("read pipe1"); goto cleanup; }
        if (bytes == 0) {
            /* Допускаем последнюю команду без перевода строки перед EOF. */
            if (parser.in_token || parser.have_number || parser.invalid) {
                if (save_sum(file, &parser) < 0) goto cleanup;
            }
            break;
        }

        /* разбираем числа и записываем сумму на каждом \n.
         * parser сохраняется между read: число/строка могут быть разбиты. */
        for (ssize_t i = 0; i < bytes; ++i) {
            if (buf[i] == '\n') {
                if (save_sum(file, &parser) < 0) goto cleanup;
            } else consume(&parser, buf[i]);
        }

        /* возвращаем полученный блок родителю.
           Эхо означает получение блока, а не завершение всей команды. */
        if (write_all(STDOUT_FILENO, buf, (size_t)bytes) < 0) {
            report_error("write pipe2"); goto cleanup;
        }
    }
    result = 0;
cleanup:
    if (close_fd(&file) < 0) result = 1;
    return result;
}
