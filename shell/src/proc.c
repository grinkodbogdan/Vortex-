#include "proc.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

pid_t proc_spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        setsid();
        execvp(argv[0], argv);
        fprintf(stderr, "vortex: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

int proc_poll(pid_t pid, int *code)
{
    int st;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0)
        return 0;
    if (r < 0)
        return -1;
    if (code)
        *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return 1;
}

void proc_kill_group(pid_t pid, int grace_ms)
{
    struct timespec ts = { 0, 20 * 1000000 };
    if (pid <= 0)
        return;
    kill(-pid, SIGTERM);
    for (int waited = 0; waited < grace_ms; waited += 20) {
        if (proc_poll(pid, NULL) != 0)
            return;
        nanosleep(&ts, NULL);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
