#define _GNU_SOURCE
#include "spawn_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>

extern char **environ;

static volatile sig_atomic_t g_child_pid = 0;
static volatile sig_atomic_t g_got_sigint = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_got_sigint = 1;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGINT);
    }
}

int distill_spawn(char *const argv[], const char *cwd, char *const envp[],
                  int stdin_fd, int stdout_fd, int stderr_fd, pid_t *out_pid) {
    if (!argv || !argv[0]) {
        return EINVAL;
    }

    posix_spawn_file_actions_t fa;
    int err = posix_spawn_file_actions_init(&fa);
    if (err != 0) return err;

    if (cwd != NULL) {
        posix_spawn_file_actions_addchdir_np(&fa, cwd);
    }

    if (stdin_fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, stdin_fd, STDIN_FILENO);
    }
    if (stdout_fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, stdout_fd, STDOUT_FILENO);
    }
    if (stderr_fd >= 0) {
        posix_spawn_file_actions_adddup2(&fa, stderr_fd, STDERR_FILENO);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    char *const *effective_env = envp ? envp : environ;
    pid_t pid = 0;
    err = posix_spawnp(&pid, argv[0], &fa, &attr, argv, effective_env);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (err == 0 && out_pid) {
        *out_pid = pid;
    }
    return err;
}

int distill_spawn_wait(pid_t pid, int *out_exit_status) {
    if (pid <= 0) return -1;

    g_child_pid = pid;
    g_got_sigint = 0;

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);

    int status = 0;
    pid_t res;
    do {
        res = waitpid(pid, &status, 0);
    } while (res == -1 && errno == EINTR && !g_got_sigint);

    sigaction(SIGINT, &old_sa, NULL);
    g_child_pid = 0;

    if (res == -1) {
        return -1;
    }

    if (out_exit_status) {
        if (WIFEXITED(status)) {
            *out_exit_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *out_exit_status = 128 + WTERMSIG(status);
        } else {
            *out_exit_status = -1;
        }
    }

    if (g_got_sigint) {
        raise(SIGINT);
    }

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}

int distill_spawn_sync(char *const argv[], const char *cwd, char *const envp[]) {
    pid_t pid = 0;
    int err = distill_spawn(argv, cwd, envp, -1, -1, -1, &pid);
    if (err != 0) {
        return -err;
    }
    int status = 0;
    distill_spawn_wait(pid, &status);
    return status;
}

int distill_spawn_capture_stdout(char *const argv[], const char *cwd, char *const envp[],
                                 char **out_buf, size_t *out_size, int *out_status) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = 0;
    int err = distill_spawn(argv, cwd, envp, -1, pipefd[1], -1, &pid);
    close(pipefd[1]); // Close write end in parent
    if (err != 0) {
        close(pipefd[0]);
        return -err;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        return -1;
    }

    ssize_t n;
    char temp[4096];
    while ((n = read(pipefd[0], temp, sizeof(temp))) > 0) {
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(pipefd[0]);
                return -1;
            }
            buf = nb;
        }
        memcpy(buf + len, temp, n);
        len += n;
    }
    close(pipefd[0]);
    buf[len] = '\0';

    int status = 0;
    distill_spawn_wait(pid, &status);
    if (out_status) *out_status = status;

    if (out_buf) *out_buf = buf;
    else free(buf);
    if (out_size) *out_size = len;

    return status;
}
