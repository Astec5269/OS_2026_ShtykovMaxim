#define _POSIX_C_SOURCE 200809L
#include <sys/wait.h>
#include <signal.h>
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


/* Получаем эхо всех переданных байтов, даже если read разбил ответ. */
static int receive_echo(int fd, size_t count) {
    char buf[4096];
    while (count) {
        size_t want = count < sizeof(buf) ? count : sizeof(buf);
        ssize_t bytes = read(fd, buf, want);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) { report_error("read pipe2"); return -1; }
        if (bytes == 0) {
            (void)put_text(2, "Child closed its output pipe.\n"); return -1;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)bytes) < 0) {
            report_error("write terminal"); return -1;
        }
        count -= (size_t)bytes;
    }
    return 0;
}

int main(void) {
    int pipe1[2] = {-1, -1}, pipe2[2] = {-1, -1};
    int result = 1, status;
    pid_t pid = -1;
    char filename[PATH_MAX], child_path[PATH_MAX], c;
    size_t length = 0;

    /* При закрытом канале write вернёт EPIPE вместо гибели от SIGPIPE. */
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGPIPE, &action, NULL) < 0) {
        report_error("sigaction");
        return 1;
    }

    if (put_text(1, "Output filename: ") < 0) {
        report_error("write terminal");
        return 1;
    }
    for (;;) {
        int r = read_byte(0, &c);
        if (r < 0) { report_error("read filename"); return 1; }
        if (r == 0 || c == '\n') break;
        if (c == '\0' || length + 1 >= sizeof(filename)) {
            (void)put_text(2, "Invalid or too long filename.\n");
            return 1;
        }
        filename[length++] = c;
    }
    if (length && filename[length - 1] == '\r') --length;
    filename[length] = '\0';
    if (!length) { (void)put_text(2, "Empty filename.\n"); return 1; }

    /* Ubuntu/Linux: ищем child рядом с parent, независимо от рабочей папки. */
    ssize_t n = readlink("/proc/self/exe", child_path, sizeof(child_path) - 1);
    if (n < 0) { report_error("readlink"); return 1; }
    if ((size_t)n >= sizeof(child_path) - 1) {
        (void)put_text(2, "Executable path too long.\n"); return 1;
    }
    size_t pos = (size_t)n;
    while (pos && child_path[pos - 1] != '/') --pos;
    const char suffix[] = "child";
    if (pos + sizeof(suffix) > sizeof(child_path)) return 1;
    for (size_t i = 0; i < sizeof(suffix); ++i) child_path[pos + i] = suffix[i];

    if (pipe(pipe1) < 0) { report_error("pipe1"); goto cleanup; }
    if (pipe(pipe2) < 0) { report_error("pipe2"); goto cleanup; }
    pid = fork();
    switch (pid) {
    case -1:
        report_error("fork");
        goto cleanup;
    case 0: {
        /* Дочерняя ветка пока выполняет код parent. */
        if (close_fd(&pipe1[1]) < 0 || close_fd(&pipe2[0]) < 0) _exit(1);
        if (dup2(pipe1[0], STDIN_FILENO) < 0 ||
            dup2(pipe2[1], STDOUT_FILENO) < 0) {
            report_error("dup2"); _exit(1);
        }
        if (close_fd(&pipe1[0]) < 0 || close_fd(&pipe2[1]) < 0) _exit(1);
        char *const args[] = {"child", filename, NULL};
        execv(child_path, args);
        /* При успехе execv сюда не возвращается. */
        report_error("exec child");
        _exit(1);
    }

    default:
        break; /* Ниже работает только родитель. */
    }

    if (close_fd(&pipe1[0]) < 0 || close_fd(&pipe2[1]) < 0) goto cleanup;
    if (put_text(1, "Enter integers, one command per line. Empty line = finish.\n") < 0) {
        report_error("write terminal"); goto cleanup;
    }

    char buf[4096];
    int line_started = 0, stop = 0;
    while (!stop) {
        ssize_t bytes = read(STDIN_FILENO, buf, sizeof(buf));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) { report_error("read terminal"); goto cleanup; }
        if (bytes == 0) break;

        /* пустая строка завершает работу.
           Проверяем её и в середине блока, и на границе двух чтений. */
        size_t count = 0;
        while (count < (size_t)bytes) {
            char current = buf[count];
            if (current == '\n' && !line_started) { stop = 1; break; }
            line_started = current != '\n';
            ++count;
        }
        if (!count) break;
        if (write_all(pipe1[1], buf, count) < 0) {
            report_error("write pipe1"); goto cleanup;
        }
        /* Ребёнок возвращает полученный текст, как эхо.
           Вычисленные суммы он записывает только в файл. */
        if (receive_echo(pipe2[0], count) < 0) goto cleanup;
    }
    result = 0;

cleanup:
    /* Закрытие pipe1[1] позволяет ребёнку получить EOF. */
    if (close_fd(&pipe1[0]) < 0) result = 1;
    if (close_fd(&pipe1[1]) < 0) result = 1;
    if (close_fd(&pipe2[0]) < 0) result = 1;
    if (close_fd(&pipe2[1]) < 0) result = 1;
    if (pid > 0) {
        pid_t w;
        do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
        if (w < 0) { report_error("waitpid"); result = 1; }
        else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            (void)put_text(2, "Child did not finish successfully.\n");
            result = 1;
        }
    }
    return result;
}
