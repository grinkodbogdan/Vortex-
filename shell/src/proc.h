#ifndef VX_PROC_H
#define VX_PROC_H

#include <sys/types.h>

/* Start argv[0] (searched in PATH) in its own session so the whole
 * process group can be signalled.  Returns the pid or -1. */
pid_t proc_spawn(char *const argv[]);
/* Non-blocking reap.  Returns 1 and the exit code (or 128+signal) when the
 * child has finished, 0 while it still runs, -1 on error. */
int proc_poll(pid_t pid, int *code);
/* SIGTERM the group, then SIGKILL after grace_ms if it is still alive. */
void proc_kill_group(pid_t pid, int grace_ms);

#endif
