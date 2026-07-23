/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/cmd_exec.c — the /exec chat command + its Unix process / fd-watch /
 * signal machinery.
 *
 * Compiled ONLY on non-Windows hosts (src/meson.build gates it): it uses
 * fork/pipe/dup2/execve, POSIX signals, and a GIOChannel watch over a raw
 * fd (g_io_channel_unix_new) — none of which exist on Windows. See
 * cmd_exec.h for the rationale and the two exported entry points.
 *
 * This all used to live inline in gtkhx.c (fd table + signal handlers)
 * and commands.c (/exec itself); it moved here so the portable C files
 * don't carry Unix-only code behind #ifdefs.
 */

#include "config.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h> /* FD_SETSIZE */
#include <sys/types.h>
#include <sys/wait.h>
#if !defined(_SC_OPEN_MAX)
#include <sys/resource.h>
#endif

#include <gtk/gtk.h>
#include "hx.h"
#include "gtkhx_log.h" /* hx_printf / hx_printf_prefix / INFOPREFIX */
#include "chat.h"      /* hx_send_chat */
#include "cmd_exec.h"

/* ---- fd-watch table (GIOChannel over the /exec output pipe) --------- */

/* Environment for execve; the fd table + its size. Declared extern in
 * protocol.h — the storage lives here now (Unix-only). */
char **hxd_environ = 0;
int hxd_open_max = 0;
struct hxd_file *hxd_files = 0;

/* GLib source ids for each fd's read / write watch, or -1 when unwatched.
 * Sized to the same 1024-fd ceiling hxd_fd_set enforces. */
static int rinput_tags[1024];
static int winput_tags[1024];

static gboolean
hxd_gtk_read (GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
    (void) source;
    (void) cond;
    if (file->ready_read) {
        file->ready_read (file->fd);
    }
    return TRUE;
}

static gboolean
hxd_gtk_write (GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
    (void) source;
    (void) cond;
    if (file->ready_write) {
        file->ready_write (file->fd);
    }
    return TRUE;
}

void
hxd_fd_set (int fd, int rw)
{
    int tag, type = 0;
    GIOChannel *channel;

    if (fd >= 1024) {
        hx_printf_prefix (hx_active_session ()->htlc, 0, INFOPREFIX,
                          "gtkhx: fd %d >= 1024", fd);
        hx_quit ();
    }

    channel = g_io_channel_unix_new (fd);
    if (rw & FDR) {
        if (rinput_tags[fd] != -1) {
            return;
        }
        type |= G_IO_IN | G_IO_HUP | G_IO_ERR;
        tag = g_io_add_watch (channel, type, (GIOFunc)hxd_gtk_read,
                              &hxd_files[fd]);
        rinput_tags[fd] = tag;
    }
    if (rw & FDW) {
        if (winput_tags[fd] != -1) {
            return;
        }
        type |= G_IO_OUT | G_IO_ERR;
        tag = g_io_add_watch (channel, type, (GIOFunc)hxd_gtk_write,
                              &hxd_files[fd]);
        winput_tags[fd] = tag;
    }
}

void
hxd_fd_clr (int fd, int rw)
{
    int tag;

    if (fd >= 1024) {
        hx_printf_prefix (hx_active_session ()->htlc, 0, INFOPREFIX,
                          "gtkhx: fd %d >= 1024", fd);
        hx_quit ();
    }
    /* The arrays are pre-zeroed to -1 (see hxd_exec_init), so a clear
     * request for a fd that was never set up would otherwise call
     * g_source_remove((guint)-1) and trip GLib's "Source ID … was not
     * found" critical. */
    if ((rw & FDR) && rinput_tags[fd] != -1) {
        tag = rinput_tags[fd];
        g_source_remove (tag);
        rinput_tags[fd] = -1;
    }
    if ((rw & FDW) && winput_tags[fd] != -1) {
        tag = winput_tags[fd];
        g_source_remove (tag);
        winput_tags[fd] = -1;
    }
}

/* ---- signal handlers ----------------------------------------------- */

#ifndef WAIT_ANY
#define WAIT_ANY -1
#endif

static RETSIGTYPE
sig_chld (int sig)
{
    int status, serrno = errno;
    pid_t pid;

    (void) sig;
    /* Reap every finished /exec child so they don't linger as zombies.
     * There's no per-pid bookkeeping to do beyond the waitpid itself. */
    for (;;) {
        pid = waitpid (WAIT_ANY, &status, WNOHANG);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!pid) {
            break;
        }
    }
    errno = serrno;
}

static RETSIGTYPE
sig_bus (int sig)
{
    (void) sig;
    abort ();
}

static RETSIGTYPE
sig_fpe (int sig)
{
    g_error ("SIGFPE (%d)", sig);
    abort ();
}

/* ---- /exec command ------------------------------------------------- */

static void
exec_close (int fd)
{
    close (fd);
    memset (&hxd_files[fd], 0, sizeof (struct hxd_file));
    hxd_fd_clr (fd, FDR);
}

static void
exec_ready_read (int fd)
{
    ssize_t r;
    char buf[0x4000];

    r = read (fd, buf, sizeof (buf) - 1);
    if (r == 0 || (r < 0 && errno != EINTR)) {
        exec_close (fd);
    } else {
        buf[r] = 0;
        /* conn.htlc holds the connection that started the command in
         * `/exec -o` mode (see the setter in cmd_exec), or NULL for a
         * plain `/exec` whose output prints locally. Route -o output back
         * to that originating connection — not hx_active_session(), which
         * can differ from it once multiple connections exist. */
        struct htlc_conn *out_htlc = hxd_files[fd].conn.htlc;
        if (out_htlc) {
            LF2CR (buf, r);
            if (buf[r - 1] == '\r') {
                buf[r - 1] = 0;
            }
            hx_send_chat (out_htlc, buf, hxd_files[fd].cid, 0);
        } else {
            hx_printf (hx_active_session ()->htlc, hxd_files[fd].cid, "%s", buf);
        }
    }
}

void
cmd_exec (int argc, char **argv, char *str, struct htlc_conn *htlc, guint32 cid)
{
    int pfds[2];
    char *p, *av[4];
    guint32 output_to = 0;

    if (argc < 2) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s [-o] <command>\n",
                          argv[0]);
        return;
    }
    p = str;
find_cmd_arg:
    for (; *p && *p != ' '; p++)
        ;
    if (!*p || !(*++p)) {
        return;
    }
    if (*p == '-' && *(p + 1) == 'o') {
        output_to = 1;
        goto find_cmd_arg;
    }
    if (pipe (pfds)) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: pipe: %s\n", argv[0],
                          strerror (errno));
        return;
    }
    if (pfds[0] >= hxd_open_max) {
        hx_printf_prefix (htlc, cid, INFOPREFIX,
                          "%s:%d: %d >= hxd_open_max (%d)\n", __FILE__,
                          __LINE__, pfds[0], hxd_open_max);
        close (pfds[0]);
        close (pfds[1]);
        return;
    }
    switch (fork ()) {
    case -1:
        hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: fork: %s\n", argv[0],
                          strerror (errno));
        close (pfds[0]);
        close (pfds[1]);
        return;
    case 0:
        /* In the shell-pipe child: discard stdin (no terminal input
         * plumbed through), close our read end of the pipe, then dup the
         * write end into stdout + stderr.
         *
         * close(1)/close(2) before dup2 is redundant — POSIX dup2
         * implicitly closes the target slot if it was open, and does so
         * atomically with the assignment. The redundant pair also tripped
         * GCC -fanalyzer's -Wanalyzer-fd-use-without-check. */
        close (0);
        close (pfds[0]);
        av[0] = "/bin/sh";
        av[1] = "-c";
        av[2] = p;
        av[3] = 0;
        /* GCC -fanalyzer's -Wanalyzer-fd-leak flags every dup2 + execve
         * idiom: it models dup2 as "opens a new fd, you must close it" and
         * doesn't see execve as a process-replacement that inherits
         * descriptors. The duped fds (now 1 and 2) ARE supposed to outlive
         * this stack frame — they become the child's stdout + stderr.
         * Suppress locally; the pattern is standard POSIX. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
        if (dup2 (pfds[1], 1) == -1 || dup2 (pfds[1], 2) == -1) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: dup2: %s\n", argv[0],
                              strerror (errno));
            _exit (1);
        }
        /* The duped fds (now 1 and 2) keep the pipe open; the original
         * pfds[1] is redundant. Close it so the child doesn't carry an
         * extra reference to the write end across execve. */
        close (pfds[1]);
        execve ("/bin/sh", av, hxd_environ);
#pragma GCC diagnostic pop
        hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: execve: %s\n", argv[0],
                          strerror (errno));
        _exit (127);
    default:
        close (pfds[1]);
        if (output_to) {
            hxd_files[pfds[0]].conn.htlc = htlc;
        } else {
            hxd_files[pfds[0]].conn.htlc = 0;
        }
        hxd_files[pfds[0]].fd = pfds[0];
        hxd_files[pfds[0]].cid = cid;
        hxd_files[pfds[0]].ready_read = exec_ready_read;
        hxd_fd_set (pfds[0], FDR);
        break;
    }
}

/* ---- startup wiring ------------------------------------------------ */

void
hxd_exec_init (char **envp)
{
    struct sigaction act;
    int i;

    for (i = 0; i < 1024; i++) {
        rinput_tags[i] = -1;
        winput_tags[i] = -1;
    }

#if defined(_SC_OPEN_MAX)
    hxd_open_max = sysconf (_SC_OPEN_MAX);
#elif defined(RLIMIT_NOFILE)
    {
        struct rlimit rlimit;

        if (getrlimit (RLIMIT_NOFILE, &rlimit)) {
            exit (1);
        }
        hxd_open_max = rlimit.rlim_max;
    }
#elif defined(HAVE_GETDTABLESIZE)
    hxd_open_max = getdtablesize ();
#elif defined(OPEN_MAX)
    hxd_open_max = OPEN_MAX;
#else
    hxd_open_max = 16;
#endif
    if (hxd_open_max > FD_SETSIZE) {
        hxd_open_max = FD_SETSIZE;
    }
    hxd_files = g_malloc0 (hxd_open_max * sizeof (struct hxd_file));

    hxd_environ = envp;

    act.sa_handler = SIG_IGN;
    act.sa_flags = 0;
    sigemptyset (&act.sa_mask);
    sigaction (SIGPIPE, &act, 0);
    sigaction (SIGHUP, &act, 0);
    act.sa_handler = sig_fpe;
    sigaction (SIGFPE, &act, 0);
    act.sa_handler = sig_bus;
    sigaction (SIGBUS, &act, 0);
    act.sa_handler = sig_chld;
    act.sa_flags |= SA_NOCLDSTOP;
    sigaction (SIGCHLD, &act, 0);
}
