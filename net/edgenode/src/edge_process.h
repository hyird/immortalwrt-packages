#pragma once

/* Runs argv without a shell. Negative file descriptors keep stdin/stdout unchanged. */
int edge_process_run(const char *const argv[], int stdin_fd, int stdout_fd);
int edge_process_run_timeout(const char *const argv[], int stdin_fd, int stdout_fd,
                             unsigned timeout_ms);

/*
 * Double-forks a detached worker. Returns 1 in the original parent, 0 in the
 * detached worker, and -1 on failure. The short-lived intermediate child is
 * always reaped before the parent returns.
 */
int edge_process_detach(void);

/* Closes every descriptor >= 3 except keep_fd in a forked worker. */
void edge_process_close_inherited_fds(int keep_fd);
