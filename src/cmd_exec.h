/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * src/cmd_exec.h — the /exec chat command and its supporting Unix
 * process / fd-watch / signal machinery.
 *
 * /exec runs a shell command in a POSIX subprocess (fork + pipe +
 * /bin/sh) and streams its output into chat through a GIOChannel watch
 * on the pipe read end. All of that — the fd-watch table (hxd_fd_set /
 * hxd_fd_clr over hxd_files), the SIGCHLD reaper, and the crash-signal
 * handlers — is inherently Unix-only, so it lives in src/cmd_exec.c,
 * which the build compiles ONLY on non-Windows hosts (see src/meson.build).
 *
 * The two symbols below are the module's entry points into the
 * otherwise-portable code:
 *   - cmd_exec       — the command-table handler (referenced from
 *                      commands.c's table, guarded by G_OS_UNIX there).
 *   - hxd_exec_init  — one-time startup wiring (fd table alloc, signal
 *                      handlers), called from main() under G_OS_UNIX.
 *
 * The fd-table declarations themselves (hxd_files, hxd_fd_set, …) stay in
 * protocol.h since the struct is platform-neutral; only the definitions
 * are Unix-gated here.
 */

#ifndef GTKHX_CMD_EXEC_H
#define GTKHX_CMD_EXEC_H 1

#include "protocol.h" /* struct htlc_conn */

#define FDR 1
#define FDW 2

/* The /exec command handler (COMMAND-macro signature). Non-static so
 * commands.c's dispatch table can reference it. */
extern void cmd_exec (int argc, char **argv, char *str,
                      struct htlc_conn *htlc, guint32 cid);

/* Startup wiring for the /exec machinery: sizes + allocates the fd
 * table, zeroes the GIOChannel-watch tag arrays, stashes the environment
 * (for execve), and installs the SIGPIPE/SIGHUP/SIGFPE/SIGBUS/SIGCHLD
 * handlers. Call once, early in main(). `envp` is main()'s environment. */
extern void hxd_exec_init (char **envp);

#endif /* GTKHX_CMD_EXEC_H */
