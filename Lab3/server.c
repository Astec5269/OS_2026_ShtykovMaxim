#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include <sys/wait.h>
#include <limits.h>
#include <string.h>

/* Имена содержат PID и время запуска; O_EXCL исключает захват чужого IPC. */
static size_t append_uint(char *out, size_t pos, unsigned long long value) {
    char digits[32];
    size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) out[pos++] = digits[--n];
    return pos;
}

static void make_name(char name[128], char kind, const struct timespec *stamp) {
    const char prefix[] = "/sum_";
    size_t n = sizeof(prefix) - 1;
    memcpy(name, prefix, n);
    n = append_uint(name, n, (unsigned long long)getpid());
    name[n++] = '_';
    n = append_uint(name, n, (unsigned long long)stamp->tv_sec);
    name[n++] = '_';
    n = append_uint(name, n, (unsigned long long)stamp->tv_nsec);
    name[n++] = '_'; name[n++] = kind; name[n] = '\0';
}

/* Обычное sem_wait не разбудит родителя при аварии/ошибке exec ребёнка.
   Проверяем waitpid раз в секунду, только пока нет подтверждения. */
static int wait_reply(sem_t *reply, pid_t *child) {
    for (;;) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) < 0) return -1;
        ++deadline.tv_sec;
        if (sem_timedwait(reply, &deadline) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) return -1;
        int status;
        pid_t w = waitpid(*child, &status, WNOHANG);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return -1;
        if (w > 0) {
            *child = -1;
            errno = ECHILD;
            return -1;
        }
    }
}

static int exchange(Shared *shared, sem_t *request, sem_t *reply,
                    pid_t *child, uint32_t length) {
    shared->length = length;
    if (sem_post(request) < 0) { report_error("sem_post request"); return -1; }
    if (wait_reply(reply, child) < 0) { report_error("wait reply"); return -1; }
    if (shared->status) {
        (void)put_text(2, "Client reported an error.\n"); return -1;
    }
    if (length != END_DATA && write_all(1, shared->data, length) < 0) {
        report_error("write echo"); return -1;
    }
    return 0;
}

int main(void) {
    int result = 1, shm = -1;
    int have_shm = 0, have_request = 0, have_reply = 0;
    pid_t child = -1;
    Shared *shared = MAP_FAILED;
    sem_t *request = SEM_FAILED, *reply = SEM_FAILED;
    char shm_name[128], request_name[128], reply_name[128];
    char filename[PATH_MAX], client_path[PATH_MAX], c;
    size_t length = 0;

    /* Ошибка вывода в закрытый канал должна попасть в обработку ошибок. */
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGPIPE, &action, NULL) < 0) {
        report_error("sigaction"); return 1;
    }
    if (put_text(1, "Output filename: ") < 0) {
        report_error("write prompt"); return 1;
    }
    for (;;) {
        int r = read_byte(0, &c);
        if (r < 0) { report_error("read filename"); return 1; }
        if (!r || c == '\n') break;
        if (c == '\0' || length + 1 >= sizeof(filename)) {
            (void)put_text(2, "Invalid or too long filename.\n"); return 1;
        }
        filename[length++] = c;
    }
    if (length && filename[length - 1] == '\r') --length;
    filename[length] = '\0';
    if (!length) { (void)put_text(2, "Empty filename.\n"); return 1; }

    ssize_t n = readlink("/proc/self/exe", client_path, sizeof(client_path) - 1);
    if (n < 0) { report_error("readlink"); return 1; }
    if ((size_t)n >= sizeof(client_path) - 1) {
        (void)put_text(2, "Executable path too long.\n"); return 1;
    }
    size_t pos = (size_t)n;
    while (pos && client_path[pos - 1] != '/') --pos;
    const char suffix[] = "client";
    if (pos + sizeof(suffix) > sizeof(client_path)) return 1;
    memcpy(client_path + pos, suffix, sizeof(suffix));

    struct timespec stamp;
    if (clock_gettime(CLOCK_REALTIME, &stamp) < 0) {
        report_error("clock_gettime"); return 1;
    }
    make_name(shm_name, 'm', &stamp);
    make_name(request_name, 'q', &stamp);
    make_name(reply_name, 'r', &stamp);
    shm = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shm < 0) { report_error("shm_open server"); goto cleanup; }
    have_shm = 1;
    if (ftruncate(shm, (off_t)sizeof(Shared)) < 0) {
        report_error("ftruncate"); goto cleanup;
    }
    shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                  MAP_SHARED, shm, 0);
    if (shared == MAP_FAILED) { report_error("mmap server"); goto cleanup; }
    request = sem_open(request_name, O_CREAT | O_EXCL, 0600, 0);
    if (request == SEM_FAILED) { report_error("sem_open request"); goto cleanup; }
    have_request = 1;
    reply = sem_open(reply_name, O_CREAT | O_EXCL, 0600, 0);
    if (reply == SEM_FAILED) { report_error("sem_open reply"); goto cleanup; }
    have_reply = 1;

    child = fork();
    if (child < 0) { report_error("fork"); goto cleanup; }
    if (child == 0) {
        char *const args[] = {"client", filename, shm_name,
                              request_name, reply_name, NULL};
        execv(client_path, args);
        report_error("execv client");
        _exit(1);
    }
    if (wait_reply(reply, &child) < 0) {
        report_error("wait client startup"); goto cleanup;
    }
    if (shared->status) goto cleanup;

    /* Оба процесса уже открыли объекты. Удаляем только имена;
       память/семафоры живут до закрытия последних ссылок на них.
       Это предотвращает остатки IPC при последующем аварийном выходе. */
    if (shm_unlink(shm_name) < 0) { report_error("shm_unlink"); goto cleanup; }
    have_shm = 0;
    if (sem_unlink(request_name) < 0) { report_error("sem_unlink request"); goto cleanup; }
    have_request = 0;
    if (sem_unlink(reply_name) < 0) { report_error("sem_unlink reply"); goto cleanup; }
    have_reply = 0;

    if (put_text(1, "Enter integers, one command per line. Empty line = finish.\n") < 0) {
        report_error("write prompt"); goto cleanup;
    }
    int line_started = 0, stop = 0;
    while (!stop) {
        /* Родитель владеет буфером до sem_post(request). */
        ssize_t bytes = read(0, shared->data, DATA_SIZE);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) { report_error("read input"); goto cleanup; }
        if (!bytes) break;
        size_t count = 0;
        while (count < (size_t)bytes) {
            char current = shared->data[count];
            if (current == '\n' && !line_started) { stop = 1; break; }
            line_started = current != '\n';
            ++count;
        }
        if (count && exchange(shared, request, reply, &child, (uint32_t)count) < 0)
            goto cleanup;
    }
    if (exchange(shared, request, reply, &child, END_DATA) < 0) goto cleanup;
    result = 0;

cleanup:
    if (child > 0) {
        if (result && kill(child, SIGTERM) < 0 && errno != ESRCH)
            report_error("kill client");
        int status;
        pid_t w;
        do { w = waitpid(child, &status, 0); } while (w < 0 && errno == EINTR);
        if (w < 0) { report_error("waitpid"); result = 1; }
        else if (!WIFEXITED(status) || WEXITSTATUS(status)) result = 1;
    }
    if (request != SEM_FAILED && sem_close(request) < 0) {
        report_error("sem_close request"); result = 1;
    }
    if (reply != SEM_FAILED && sem_close(reply) < 0) {
        report_error("sem_close reply"); result = 1;
    }
    if (shared != MAP_FAILED && munmap(shared, sizeof(*shared)) < 0) {
        report_error("munmap server"); result = 1;
    }
    if (close_fd(&shm) < 0) result = 1;
    if (have_shm && shm_unlink(shm_name) < 0) {
        report_error("shm_unlink"); result = 1;
    }
    if (have_request && sem_unlink(request_name) < 0) {
        report_error("sem_unlink request"); result = 1;
    }
    if (have_reply && sem_unlink(reply_name) < 0) {
        report_error("sem_unlink reply"); result = 1;
    }
    return result;
}
