#ifndef DISTILL_SPAWN_UTIL_H
#define DISTILL_SPAWN_UTIL_H

#define _GNU_SOURCE
#include <sys/types.h>
#include <stddef.h>

/*
 * Spawns a process using posix_spawnp.
 * - argv: NULL-terminated array of arguments (argv[0] is program name searched in PATH).
 * - cwd: working directory for child (or NULL to keep parent's cwd).
 * - envp: environment array (or NULL to inherit parent's environ).
 * - stdin_fd, stdout_fd, stderr_fd: file descriptors to redirect (-1 to leave unchanged).
 * - out_pid: stores the spawned child's PID.
 * Returns 0 on success, or errno / non-zero on failure.
 */
int distill_spawn(char *const argv[], const char *cwd, char *const envp[],
                  int stdin_fd, int stdout_fd, int stderr_fd, pid_t *out_pid);

/*
 * Waits for child process pid to finish with signal propagation (SIGINT).
 * Returns child's exit status (0-255), or negative error code.
 */
int distill_spawn_wait(pid_t pid, int *out_exit_status);

/*
 * Spawns process and waits for completion.
 */
int distill_spawn_sync(char *const argv[], const char *cwd, char *const envp[]);

/*
 * Spawns process and captures its stdout into a dynamically allocated buffer.
 * *out_buf must be freed by caller.
 */
int distill_spawn_capture_stdout(char *const argv[], const char *cwd, char *const envp[],
                                 char **out_buf, size_t *out_size, int *out_status);

#endif /* DISTILL_SPAWN_UTIL_H */
