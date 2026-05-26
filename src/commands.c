/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <ctype.h>
#include <getopt.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "sound.h"
#include "tasks.h"
#include "options.h"
#include "files.h"
#include "news.h"
#include "text_util.h"
#include "users.h"
#include "msg.h"
#include "commands.h"

void
chrexpand (char *str, int len)
{
    char *p;
    int off;

    p = str;
    if (*p != '\\') {
        return;
    }
    off = 1;
    switch (p[1]) {
    case 'r':
        p[1] = '\r';
        break;
    case 'n':
        p[1] = '\n';
        break;
    case 't':
        p[1] = '\t';
        break;
    case 'x':
        while (isxdigit (p[off + 1]) && off < 3) {
            off++;
        }
        p[off] = (char)strtoul (p + 2, 0, 16);
        break;
    default:
        if (!isdigit (p[1]) || p[1] >= '8') {
            break;
        }
        while ((isdigit (p[off + 1]) && p[off + 1] < '8') && off < 3) {
            off++;
        }
        p[off] = (char)strtoul (p + 2, 0, 8);
        break;
    }
    len -= off;
    memcpy (p, p + off, len);
    p[len] = 0;
}

#define COMMAND(x)                                                             \
    static void cmd_##x (int argc, char **argv, char *str,                     \
                         struct htlc_conn *htlc, guint32 cid)

COMMAND (help)
{
    /* XXX: this needs to be maintained ... */

    hx_printf_prefix (htlc, cid, INFOPREFIX, "Commands: \n");
    hx_printf (htlc, cid, "clear   close     exec    help\n");
    hx_printf (htlc, cid, "icon    ignore    me      news\n");
    hx_printf (htlc, cid, "nick    post      quit    server\n");
}

COMMAND (stats)
{
    char stat_str[256];
    int years, days, hours, mins;
    time_t secs = total_time + (time (NULL) - start_time);

    years = secs / (60 * 60 * 24 * 365);
    secs %= (60 * 60 * 24 * 365);
    days = secs / (60 * 60 * 24);
    secs %= (60 * 60 * 24);
    hours = secs / (60 * 60);
    secs %= (60 * 60);
    mins = secs / 60;
    secs %= 60;

    g_snprintf (stat_str, sizeof (stat_str),
                "has been online for %d years, %d days, %02d hours, "
                "%02d mins, %02lu secs.",
                years, days, hours, mins, secs);
    hx_send_chat (htlc, stat_str, cid, 1);
}

COMMAND (nick)
{
    if (argc < 2) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <nickname>\n",
                          argv[0]);
        return;
    }
    strncpy (htlc->name, argv[1], 31);
    htlc->name[31] = '\0';
    hx_change_name_icon (htlc);
}

COMMAND (icon)
{
    if (!argv[1]) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <icon>\n", argv[0]);
        return;
    }
    htlc->icon = atou16 (argv[1]);
    hx_change_name_icon (htlc);
}

COMMAND (quit)
{
    hx_quit ();
}

static struct option server_opts[]
    = { { "login", 1, 0, 'l' }, { "password", 1, 0, 'p' }, { 0, 0, 0, 0 } };

COMMAND (server)
{
    u_int16_t port = 0;
    char *serverstr = 0, *portstr = 0, *login = 0, *pass = 0;
    int o, longind;

    opterr = 0; /* don't spam stderr on bad input from chat */
    optind = 0; /* reset getopt state across invocations */
    while ((o = getopt_long (argc, argv, "l:p:", server_opts, &longind))
           != -1) {
        if (o == 0) {
            o = server_opts[longind].val;
        }
        switch (o) {
        case 'l':
            login = optarg;
            break;
        case 'p':
            pass = optarg;
            break;
        default:
            goto usage;
        }
    }

    if (optind < argc) {
        serverstr = argv[optind];
        if (optind + 1 < argc) {
            portstr = argv[optind + 1];
        }
    }

    if (!serverstr) {
    usage:
        hx_printf_prefix (htlc, cid, INFOPREFIX,
                          "usage: %s [OPTIONS] <server address>[:][port]\n"
                          "  -l, --login <login>\n"
                          "  -p, --password <password>\n",
                          argv[0]);
        return;
    }

    if (portstr) {
        port = atou16 (portstr);
    }
    if (!port) {
        port = HTLS_TCPPORT;
    }

    hx_connect (htlc, serverstr, port, login, pass, 0);
}

static u_int32_t
cmd_arg (int argn, char *str)
{
    char *p, *cur;
    char c, quote = 0;
    int argc = 0;
    u_int32_t offset = 0, length = 0;

    p = str;
    while (isspace (*p)) {
        p++;
    }
    for (cur = p; (c = *p);) {
        if (c == '\'' || c == '"') {
            if (quote == c) {
                argc++;
                if (argn == argc) {
                    p++;
                    while (isspace (*p)) {
                        p++;
                    }
                    offset = p - str;
                    p--;
                } else if (argn + 1 == argc) {
                    length = p - (str + offset);
                    break;
                } else {
                    p++;
                    while (isspace (*p)) {
                        p++;
                    }
                    p--;
                }
                quote = 0;
                cur = ++p;
            } else if (!quote) {
                quote = c;
                cur = ++p;
            }
        } else if (!quote && isspace (c)) {
            argc++;
            if (argn == argc) {
                p++;
                while (isspace (*p)) {
                    p++;
                }
                offset = p - str;
                p--;
            } else if (argn + 1 == argc) {
                length = p - (str + offset);
                break;
            } else {
                p++;
                while (isspace (*p)) {
                    p++;
                }
                p--;
            }
            cur = ++p;
        } else if (c == '\\' && *(p + 1) == ' ') {
            p += 2;
        } else {
            p++;
        }
    }
    if (p != cur) {
        argc++;
    }
    if (argn == argc && 0 && argn != 1) {
        cur--;
        offset = cur - str;
        length = strlen (cur);
    } else if (argn + 1 == argc) {
        length = p - (str + offset);
    }

    return (offset << 16) | (length & 0xffff);
}

COMMAND (msg)
{
    u_int32_t uid;
    char *name, *msg;
    struct hx_user *user = NULL;

    name = argv[1];
    if (!name) {
        goto usage;
    }
    msg = str + (cmd_arg (2, str) >> 16);
    if (!*msg) {
    usage:
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage %s <uid> <msg>\n",
                          argv[0]);
        return;
    }
    uid = atou32 (name);
    if (!uid) {
        struct chat *chat = chat_with_cid (&the_session, 0);
        user = hx_user_with_name (chat, name);
        if (!user) {
            hx_printf_prefix (htlc, cid, INFOPREFIX,
                              "%s: no such nickname %s\n", argv[0], name);
            return;
        }
        uid = user->uid;
    }
    strncpy (last_msg_nick, name, 31);
    last_msg_nick[31] = 0;

    if (!user) {
        struct chat *chat = chat_with_cid (&the_session, 0);
        user = hx_user_with_uid (chat, uid);
    }
    if (user) {
        hx_printf (htlc, 0, "[%s(%u)]-> %s", user->name, uid, msg);
    } else {
        hx_printf (htlc, 0, "[(%u)]-> %s", uid, msg);
    }

    hx_send_msg (htlc, uid, msg, strlen (msg), NULL);
}

COMMAND (me)
{
    char *p;
    guint16 style;

    for (p = str; *p && *p != ' '; p++)
        ;
    if (!*p || !(*++p)) {
        return;
    }
    style = htons (1);

    /* Phase E2/E3: chat body — same encoder as hx_send_chat. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize wire_len = 0;
    char *wire = gtkhx_text_for_wire (p, strlen (p), utf8,
                                      /*is_body=*/TRUE, &wire_len);

    if (cid) {
        guint32 cidh = htonl (cid);
        hlwrite (htlc, HTLC_HDR_CHAT, 0, 3, HTLC_DATA_STYLE, 2, &style,
                 HTLC_DATA_CHAT, (guint16)wire_len, wire, HTLC_DATA_CHAT_ID, 4,
                 &cidh);
    } else {
        hlwrite (htlc, HTLC_HDR_CHAT, 0, 2, HTLC_DATA_STYLE, 2, &style,
                 HTLC_DATA_CHAT, (guint16)wire_len, wire);
    }
    g_free (wire);
}

COMMAND (post)
{
    char *p;

    for (p = str; *p && *p != ' '; p++)
        ;
    if (!*p || !(*++p)) {
        return;
    }
    hx_post_news (htlc, p, strlen (p));
}

COMMAND (close)
{
    if (htlc->fd) {
        hx_htlc_close (htlc, 1);
    }
}

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
        if (hxd_files[fd].conn.htlc == &the_session.htlc) {
            LF2CR (buf, r);
            if (buf[r - 1] == '\r') {
                buf[r - 1] = 0;
            }
            hx_send_chat (&the_session.htlc, buf, hxd_files[fd].cid, 0);
        } else {
            hx_printf (&the_session.htlc, hxd_files[fd].cid, "%s", buf);
        }
    }
}

COMMAND (exec)
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
        /* In the shell-pipe child: discard stdin (no terminal
		 * input plumbed through), close our read end of the pipe,
		 * then dup the write end into stdout + stderr.
		 *
		 * close(1)/close(2) before dup2 is redundant — POSIX dup2
		 * implicitly closes the target slot if it was open, and
		 * does so atomically with the assignment. The redundant
		 * pair also tripped GCC -fanalyzer's
		 * -Wanalyzer-fd-use-without-check (it tracks the close
		 * and then sees the literal `1`/`2` arg to dup2 as a
		 * use-after-close). */
        close (0);
        close (pfds[0]);
        av[0] = "/bin/sh";
        av[1] = "-c";
        av[2] = p;
        av[3] = 0;
        /* GCC -fanalyzer's -Wanalyzer-fd-leak flags every dup2 + execve
		 * idiom: it models dup2 as "opens a new fd, you must close
		 * it" and doesn't see execve as a process-replacement that
		 * inherits descriptors. The duped fds (now 1 and 2) ARE
		 * supposed to outlive this stack frame — they become the
		 * child's stdout + stderr. Suppress locally; the pattern is
		 * standard POSIX and the analyzer just lacks the model. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
        if (dup2 (pfds[1], 1) == -1 || dup2 (pfds[1], 2) == -1) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: dup2: %s\n", argv[0],
                              strerror (errno));
            _exit (1);
        }
        /* The duped fds (now 1 and 2) keep the pipe open; the
		 * original pfds[1] is redundant. Close it so the child
		 * doesn't carry an extra reference to the write end across
		 * execve. */
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

COMMAND (ignore)
{
    guint32 uid;
    struct hx_user *user = 0;
    struct chat *chat = chat_with_cid (&the_session, 0);

    if (argc < 2) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <uid>\n", argv[0]);
        return;
    }
    if (!chat) {
        hx_printf_prefix (
            htlc, cid, INFOPREFIX,
            "%s: public chat does not exist.  are you connected?\n", argv[0]);
        return;
    }

    uid = atou32 (argv[1]);
    if (!uid) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <uid>\n", argv[0]);
        return;
    }
    user = hx_user_with_uid (chat, uid);
    if (!user) {
        hx_printf_prefix (htlc, cid, INFOPREFIX,
                          "%s: no such user with uid %d\n", argv[0], uid);
        return;
    }
    user->ignore ^= 1;

    hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: %s is now %s", argv[0],
                      user->name, user->ignore ? "ignored" : "unignored");
}

COMMAND (clear)
{
    hx_clear_chat (htlc, cid, 0);
}

struct hx_command {
    char *name;
    void (*fun) (int argc, char **argv, char *str, struct htlc_conn *htlc,
                 guint32 cid);
};

static struct hx_command *commands, *last_command;

static struct hx_command commands_tbl[] = {
    { "clear", cmd_clear }, { "close", cmd_close },    { "exec", cmd_exec },
    { "help", cmd_help },   { "icon", cmd_icon },      { "ignore", cmd_ignore },
    { "me", cmd_me },       { "msg", cmd_msg },        { "nick", cmd_nick },
    { "post", cmd_post },   { "quit", cmd_quit },      { "server", cmd_server },
    { "stats", cmd_stats }, { "unignore", cmd_ignore }
};

static struct hx_command *commands
    = commands_tbl,
    *last_command
    = commands_tbl + sizeof (commands_tbl) / sizeof (struct hx_command) - 1;

static short command_hash[26];

void
gen_command_hash (void)
{
    int i, n;
    struct hx_command *cmd;

    cmd = commands;
    for (n = 0, i = 0; i < 26; i++) {
        if (cmd->name[0] == i + 'a') {
            command_hash[i] = n;
            do {
                if (++cmd > last_command) {
                    for (i++; i < 26; i++) {
                        command_hash[i] = -1;
                    }
                    return;
                }
                n++;
            } while (cmd->name[0] == i + 'a');
        } else {
            command_hash[i] = -1;
        }
    }
}

#define killspace(s)                                                           \
    while (isspace (*(s)))                                                     \
    strcpy ((s), (s) + 1)
#define add_arg(s)                                                             \
    do {                                                                       \
        argv[argc++] = s;                                                      \
        if (argc >= 16) {                                                      \
            if (argc == 16) {                                                  \
                argv = g_malloc (sizeof (char *) * 17);                        \
                memcpy (argv, auto_argv, sizeof (char *) * 16);                \
            } else                                                             \
                argv = g_realloc (argv, ((sizeof (char *) * argc) + 1));       \
        }                                                                      \
    } while (0)

void
hx_command (char *str, guint32 cid)
{
    int i;
    struct hx_command *cmd = 0;
    char *p;

    for (p = str; *p && !isspace (*p); p++)
        ;
    if (*str < 'a' || *str > 'z') {
        goto notfound;
    }
    i = *str - 'a';
    if (command_hash[i] == -1) {
        goto notfound;
    }
    cmd = commands + command_hash[i];
    do {
        if (cmd && !strncmp (str, cmd->name, p - str) && cmd->fun) {
            char *q, *cur, *s;
            char c, quote = 0;
            char *auto_argv[16], **argv = auto_argv;
            int argc = 0;

            s = g_strdup (str);
            killspace (s);
            for (q = cur = s; (c = *q); q++) {
                if (c == '\'' || c == '"') {
                    if (quote == c) {
                        *q = 0;
                        add_arg (cur);
                        killspace (q + 1);
                        quote = 0;
                        cur = q + 1;
                    } else if (!quote) {
                        quote = c;
                        cur = q + 1;
                    }
                } else if (!quote && isspace (c)) {
                    *q = 0;
                    add_arg (cur);
                    killspace (q + 1);
                    cur = q + 1;
                } else if (c == '\\') {
                    chrexpand (q, strlen (q));
                }
            }
            if (q != cur) {
                add_arg (cur);
            }
            argv[argc] = 0;

            cmd->fun (argc, argv, str, &the_session.htlc, cid);
            g_free (s);
            if (argv != auto_argv) {
                g_free (argv);
            }
            return;
        }
        cmd++;
    } while (cmd <= last_command && cmd->name[0] == *str);

notfound:
    hx_printf_prefix (&the_session.htlc, cid, INFOPREFIX,
                      "%.*s: command not found\n", (int)(p - str), str);
}
