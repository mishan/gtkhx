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
#include <ctype.h>
#include <getopt.h>
#include <gtk/gtk.h>
#include "hx.h"
#include "hxconn.h"
#include "hotline_proto.h"
#include "network.h"
#include "rcv.h"
#include "chat.h"
#include "chat_members.h" /* hx_member_model_toggle_ignore */
#include "sound.h"
#include "tasks.h"
#include "options.h"
#include "files.h"
#include "news.h"
#include "text_util.h"
#include "users.h"
#include "msg.h"
#include "commands.h"
#include "cmd_exec.h" /* cmd_exec — /exec handler (Unix-only module) */

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
    hx_printf_prefix (htlc, cid, INFOPREFIX, "Commands: \n");
    hx_printf (htlc, cid, "clear   close     exec    help\n");
    hx_printf (htlc, cid, "icon    ignore    me      news\n");
    hx_printf (htlc, cid, "nick    post      quit    server\n");
}

COMMAND (nick)
{
    if (argc < 2) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <nickname>\n",
                          argv[0]);
        return;
    }
    hx_conn_set_name (htlc, argv[1]);
    hx_change_name_icon (htlc);
}

COMMAND (icon)
{
    if (!argv[1]) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "usage: %s <icon>\n", argv[0]);
        return;
    }
    hx_conn_set_icon (htlc, atou16 (argv[1]));
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
    uint16_t port = 0;
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

    /* commands.c::cmd_connect drives connect from the in-app /connect
     * shell command — Phase 4 will add a /tls flag; for now this
     * inherits the GTKHX_TLS env var like the rest of the callers. */
    hx_connect (htlc, serverstr, port, login, pass, 0, /*tls=*/0);
}

static uint32_t
cmd_arg (int argn, char *str)
{
    char *p, *cur;
    char c, quote = 0;
    int argc = 0;
    uint32_t offset = 0, length = 0;

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
    uint32_t uid;
    char *name, *msg;

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
    struct chat *chat = chat_with_cid (sess_from_htlc (htlc), 0);
    if (!chat) {
        hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: no active chat\n",
                          argv[0]);
        return;
    }
    uid = atou32 (name);
    if (!uid) {
        uid = hx_member_model_find_by_name (hx_chat_member_model (chat), name);
        if (!uid) {
            hx_printf_prefix (htlc, cid, INFOPREFIX,
                              "%s: no such nickname %s\n", argv[0], name);
            return;
        }
    }
    strncpy (last_msg_nick, name, 31);
    last_msg_nick[31] = 0;

    struct hx_member_info mi;
    if (hx_member_model_get_info (hx_chat_member_model (chat), uid, &mi)) {
        hx_printf (htlc, 0, "[%s(%u)]-> %s", mi.name, uid, msg);
    } else {
        hx_printf (htlc, 0, "[(%u)]-> %s", uid, msg);
    }

    hx_send_msg (htlc, uid, msg, strlen (msg), NULL);
}

COMMAND (me)
{
    char *p;

    for (p = str; *p && *p != ' '; p++)
        ;
    if (!*p || !(*++p)) {
        return;
    }

    /* Phase E2/E3: chat body — same encoder as hx_send_chat. */
    gboolean utf8 = (hx_conn_has_cap (htlc, HTLC_CAP_TEXT_ENCODING)) != 0;
    gsize wire_len = 0;
    char *wire = gtkhx_text_for_wire (p, strlen (p), utf8,
                                      /*is_body=*/TRUE, &wire_len);

    /* chunk layout moved to gtkhx_proto_build_chat_chunks.
     * Same shape as hx_send_chat; the only difference is the style
     * value — /me sends style=1 (emote) vs. style=0 (normal). The
     * builder takes style as host-order and big-endian-encodes it
     * into scratch. */
    struct hx_chunk chunks[3];
    guint8 scratch[8];
    int hc = (int)gtkhx_proto_build_chat_chunks (
        cid, /*style=*/1, (const uint8_t *)wire, wire_len, chunks,
        G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        hlwrite_chunks (htlc, HTLC_HDR_CHAT, 0, chunks, hc);
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
    if (hx_conn_fd (htlc)) {
        hx_htlc_close (htlc, 1);
    }
}

COMMAND (ignore)
{
    guint32 uid;
    struct chat *chat = chat_with_cid (sess_from_htlc (htlc), 0);

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
    struct hx_member_info mi;
    if (!hx_member_model_get_info (hx_chat_member_model (chat), uid, &mi)) {
        hx_printf_prefix (htlc, cid, INFOPREFIX,
                          "%s: no such user with uid %d\n", argv[0], uid);
        return;
    }
    gboolean ig
        = hx_member_model_toggle_ignore (hx_chat_member_model (chat), uid);

    hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: %s is now %s", argv[0],
                      mi.name, ig ? "ignored" : "unignored");
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

static struct hx_command commands_tbl[]
    = { { "clear", cmd_clear },   { "close", cmd_close },
#ifdef G_OS_UNIX
        { "exec", cmd_exec }, /* /exec — Unix-only (cmd_exec.c) */
#endif
        { "help", cmd_help },     { "icon", cmd_icon },
        { "ignore", cmd_ignore }, { "me", cmd_me },
        { "msg", cmd_msg },       { "nick", cmd_nick },
        { "post", cmd_post },     { "quit", cmd_quit },
        { "server", cmd_server }, { "unignore", cmd_ignore } };

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

            cmd->fun (argc, argv, str, hx_active_session ()->htlc, cid);
            g_free (s);
            if (argv != auto_argv) {
                g_free (argv);
            }
            return;
        }
        cmd++;
    } while (cmd <= last_command && cmd->name[0] == *str);

notfound:
    hx_printf_prefix (hx_active_session ()->htlc, cid, INFOPREFIX,
                      "%.*s: command not found\n", (int)(p - str), str);
}
