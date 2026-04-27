/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <ctype.h>
#include <pwd.h>
#include "hx.h"
#include "gtk_hlist.h"
#include "macres.h"
#include "cicn.h"
#include "news.h"
#include "news15.h"
#include "users.h"
#include "files.h"
#include "tasks.h"
#include "network.h"
#include "gtkutil.h"
#include "toolbar.h"
#include "chat.h"
#include "msg.h"
#include "tracker.h"
#include "xtext.h"
#include "gtkthreads.h"
#include "options.h"
#include "toolbar.h"
#include "xfers.h"
#include "plugin.h"
#include "commands.h"
#include "getopt.h"
#include "log.h"

char last_msg_nick[32];
char *g_user_colors[4] = {WHITE_BOLD, WHITE, RED_BOLD, RED};

struct ifn user_icon_files;
struct ifn icon_files;
GtkStyle *gtktext_style = NULL;
GdkColor fg_col;
GdkColor bg_col;

GdkFont *gtkhx_font;
GtkTooltips *tooltips;
static GtkWidget *agreetext;
static struct timer *timer_list;

static int rinput_tags[1024];
static int winput_tags[1024];

const char *INFOPREFIX = " \00310[\00303hx\00310]\003 ";

session sessions[MAX_CONN];

void hx_quit (void)
{
	prefs_write();
	xfers_delete_all();
	tracker_kill_threads();

	if(sessions[0].htlc.fd) {
		hx_htlc_close(&sessions[0].htlc, 1);
	}

#if 0 /* XXX */
	close_logs();
#endif

	gtk_thread_exit();
	gtk_main_quit();
	exit(0);
}

session *sess_from_htlc(struct htlc_conn *htlc)
{
/*	int i; */
	return &sessions[0];
/*
	for(i = 0; i < MAX_CONN; i++) {
		if(&sessions[i].htlc == htlc) {
			return &sessions[i];
		}
	}
*/
	return 0;
}

void
timer_add_secs (time_t secs, int (*fn)(), void *ptr)
{
	struct timer *timer;
	guint id;

	id = gtk_timeout_add(secs * 1000, fn, ptr);

	timer = g_malloc(sizeof(struct timer));
	timer->next = 0;
	timer->prev = timer_list;
	if (timer_list)
		timer_list->next = timer;
	timer_list = timer;
	timer->id = id;
	timer->fn = fn;
	timer->ptr = ptr;
}



void
timer_delete_ptr (void *ptr)
{
	struct timer *timer;

	for (timer = timer_list; timer; timer = timer->prev) {
		if (timer->ptr == ptr) {
			if (timer->next)
				timer->next->prev = timer->prev;
			if (timer->prev)
				timer->prev->next = timer->next;
			if (timer == timer_list)
				timer_list = timer->next;
			gtk_timeout_remove(timer->id);
			g_free(timer); /* bring out yer dead! */
		}
	}
}

static gboolean hxd_gtk_read(GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
	if (file->ready_read) {
		file->ready_read(file->fd);	
	}
	return TRUE;
}

static gboolean hxd_gtk_write(GIOChannel *source, GIOCondition cond, struct hxd_file *file)
{
	if (file->ready_write) {
		file->ready_write(file->fd);
	} 
	return TRUE;
}

void hxd_fd_set (int fd, int rw)
{
	int tag, type = 0;
	GIOChannel *channel;

	if (fd >= 1024) {
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
						 "gtkhx: fd %d >= 1024", fd);
		hx_quit();
	}

	
#ifdef WIN32
	channel = g_io_channel_win32_new_socket (fd);	
#else
	channel = g_io_channel_unix_new (fd);
#endif
	if (rw & FDR) {
/*		printf("adding fd %d for reading\n", fd); */
		if(rinput_tags[fd] != -1)
			return;
		type |= G_IO_IN | G_IO_HUP | G_IO_ERR;
		tag = g_io_add_watch(channel, type, (GIOFunc)hxd_gtk_read, &hxd_files[fd]);
		rinput_tags[fd] = tag;
	}
	if (rw & FDW) {
/*		printf("adding fd %d for writing\n", fd); */
		if(winput_tags[fd] != -1)
			return;
		type |= G_IO_OUT | G_IO_ERR;
		tag = g_io_add_watch(channel, type, (GIOFunc)hxd_gtk_write, &hxd_files[fd]);
		winput_tags[fd] = tag;
	}
}

void
hxd_fd_clr (int fd, int rw)
{
	int tag;

	if (fd >= 1024) {
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
						 "gtkhx: fd %d >= 1024", fd);
		hx_quit();
	}
	if (rw & FDR) {
		tag = rinput_tags[fd];
		g_source_remove(tag);
		rinput_tags[fd] = -1;
	}
	if (rw & FDW) {
		tag = winput_tags[fd];
		g_source_remove(tag);
		winput_tags[fd] = -1;
	}
}

static void init_colors (GtkWidget *widget)
{
	GdkColormap *colormap;
	int i;
	user_colors[0].red = 0x0000;
	user_colors[0].green = 00000;
	user_colors[0].blue = 0x0000;
	user_colors[1].red = 0xffff;
	user_colors[1].green = 0x0000;
	user_colors[1].blue = 0x0000;
	user_colors[2].red = 0x0000;
	user_colors[2].green = 0xffff;
	user_colors[2].blue = 0x0000;
	user_colors[3].red = 0xffff;
	user_colors[3].green = 0xffff;
	user_colors[3].blue = 0x0000;
	user_colors[4].red = 0x0000;
	user_colors[4].green = 0x0000;
	user_colors[4].blue = 0xffff;
	user_colors[5].red = 0xffff;
	user_colors[5].green = 0x0000;
	user_colors[5].blue = 0xffff;
	user_colors[6].red = 0x0000;
	user_colors[6].green = 0xffff;
	user_colors[6].blue = 0xffff;
	user_colors[7].red = 0xffff;
	user_colors[7].green = 0xffff;
	user_colors[7].blue = 0xffff;

	colormap = gtk_widget_get_colormap(widget);
	for (i = 0; i < 8; i++) {
		user_colors[i].pixel = (gulong)(((user_colors[i].red & 0xff00) << 8) + 
										(user_colors[i].green & 0xff00) + 
										((user_colors[i].blue & 0xff00) >> 8));
		if (!gdk_colormap_alloc_color(colormap, &user_colors[i], 0, 1))
			hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
							 _("alloc color failed"));
	}

	gdk_user_colors[0].red = 0;
	gdk_user_colors[0].green = 0;
	gdk_user_colors[0].blue = 0;
	gdk_user_colors[1].red = 0xa0a0;
	gdk_user_colors[1].green = 0xa0a0;
	gdk_user_colors[1].blue = 0xa0a0;
	gdk_user_colors[2].red = 0xffff;
	gdk_user_colors[2].green = 0;
	gdk_user_colors[2].blue = 0;
	gdk_user_colors[3].red = 0xffff;
	gdk_user_colors[3].green = 0xa7a7;
	gdk_user_colors[3].blue = 0xb0b0;

	for (i = 0; i < 4; i++) {
		colors[i].pixel = (gulong)(((gdk_user_colors[i].red & 0xff00) << 8) + 
								   (gdk_user_colors[i].green & 0xff00) + 
								   ((gdk_user_colors[i].blue & 0xff00) >> 8));
		if (!gdk_colormap_alloc_color(colormap, &gdk_user_colors[i], 0, 1))
			hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
							 _("alloc color failed"));
	}
}

char *colorstr (guint16 color)
{
	char *col;

	col = g_user_colors[color % 4];

	return col;
}

void init_icons (void)
{
	int fd, i;
	struct ifn *ifn = &icon_files;

	if(ifn->cicns) {
		for(i = 0; i < ifn->n; i++) {
			if(ifn->cicns[i]) {
				macres_file_delete(ifn->cicns[i]);
				ifn->cicns[i] = 0;
			}
		}
		g_free(ifn->cicns);
		ifn->cicns = 0;
	}

	if(ifn->files) {
		for(i = 0; i < ifn->n; i++)
			g_free(ifn->files[i]);
		g_free(ifn->files);
	}

	ifn->files = g_malloc(gtkhx_prefs.num_icons*sizeof(char *));
	ifn->cicns = g_malloc(sizeof(macres_file *) * gtkhx_prefs.num_icons);

	for(i = 0; i < gtkhx_prefs.num_icons; i++) {
		ifn->files[i] = g_strdup(gtkhx_prefs.icon[i]);
		fd = open(ifn->files[i], O_RDONLY);
		if(fd < 0) {
			g_warning("%s: %s\n", ifn->files[i], strerror(errno));
			ifn->cicns[i] = 0;
			continue;
		}
		ifn->cicns[i] = macres_file_open(fd);
		close (fd);
	}
	ifn->n = gtkhx_prefs.num_icons;
}

extern void reinit_gtktexts(session *sess);

static void fe_init (void)
{
	GtkWidget *widg = gtk_button_new();

	tooltips = gtk_tooltips_new();
	generate_colors(widg);
	gtk_widget_destroy(widg);
	init_variables();

	memset(&icon_files, 0, sizeof(icon_files));
	prefs_read();
	init_icons();

	/* initialize some pointers with linked list
	   this will be handled somewhere else in case of
	   multiconnection support */

	sessions[0].chat_list = &(sessions[0].__chat_list);
	sessions[0].chat_tail = &(sessions[0].__chat_list);
	sessions[0].chat_front = &(sessions[0].__chat_list);
	sessions[0].__chat_list.user_list = &(sessions[0].__chat_list.__user_list);
	sessions[0].__chat_list.user_tail = &(sessions[0].__chat_list.__user_list);
	sessions[0].task_list = &(sessions[0].__task_list);
	sessions[0].task_tail = &(sessions[0].__task_list);

	create_toolbar_window(&sessions[0]);
	init_colors(toolbar_window);

	create_chat(&sessions[0]);
	if(gtkhx_prefs.geo.chat.init == 1)
		create_chat_window(0, &sessions[0]);
	if(gtkhx_prefs.geo.news.init == 1)
		create_news_window(&sessions[0]);
	if(gtkhx_prefs.geo.users.init == 1)
		create_users_window(0, &sessions[0]);
	create_tasks(&sessions[0]);
	if(gtkhx_prefs.geo.tasks.init == 1)
		create_tasks_window(0, &sessions[0]);

	reinit_gtktexts(&sessions[0]);
}

static void
loop (void)
{
	gtk_threads_init();
	gtk_threads_main();
}

static void init (int argc, char **argv)
{
	int i;

	for(i = 0; i < 1024; i++) {
		rinput_tags[i] = -1;
		winput_tags[i] = -1;
	}
	gtk_set_locale();
	gtk_init(&argc, &argv);
	fe_init();
}

static void output_user_info (guint16 uid, const char *nam, const char *info, 
							  guint16 len)
{
	if(len > 0) {
		GtkWidget *info_window;
		GtkWidget *info_text;
		GtkWidget *vscroll;
		GtkWidget *hbox;
		char infotitle[45];

		info_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_widget_set_usize(info_window, 260, 250);

		sprintf(infotitle, _("User Info: %s (%u)"), nam, uid);
		gtk_window_set_title(GTK_WINDOW(info_window), infotitle);

		hbox = gtk_hbox_new(0, 0);
		info_text = gtk_text_new(0, 0);
		vscroll = gtk_vscrollbar_new(GTK_TEXT(info_text)->vadj);

		gtk_box_pack_start(GTK_BOX(hbox), info_text, 1, 1, 0);
		gtk_box_pack_start(GTK_BOX(hbox), vscroll, 0, 0, 0);

		gtk_container_add(GTK_CONTAINER(info_window), hbox);
		gtk_text_insert(GTK_TEXT(info_text), 0, 0, 0, info, len);
		init_keyaccel(info_window);
		gtk_widget_show_all(info_window);
	}
}

static void output_chat (struct htlc_conn *htlc, guint32 cid, char *chat, 
						 guint16 chatlen)
{
		hx_printf(htlc, cid, "%.*s\n", chatlen, chat);
}

static void concurrence(GtkWidget *widget, gpointer data)
{
	session *sess  = data;

	gtk_widget_destroy(sess->agreementwin);
	sess->agreementwin = 0;
}

static void disagreement(GtkWidget *widget, gpointer data)
{
	session *sess = data;

	if(sess->htlc.fd)
		hx_htlc_close(&sess->htlc, 1);
}

static void output_agreement (session *sess, const char *agreement, guint16 len)
{
	GtkWidget *agreementwin;
	GtkWidget *agreebtn;
	GtkWidget *disagreebtn;
	GtkWidget *vbox;
	GtkWidget *hbox;
	GtkWidget *hboxtwo;
	GtkWidget *vscroller;

	agreementwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_set_usize(agreementwin, 400, 500);
	gtk_window_set_title(GTK_WINDOW(agreementwin), _("Agreement"));
	agreetext = gtk_text_new(FALSE, 0);
	gtk_widget_set_usize(agreetext, 382, 480);
	agreebtn = gtk_button_new_with_label(_("Agree"));
	disagreebtn = gtk_button_new_with_label(_("Disagree"));
	vbox = gtk_vbox_new(FALSE, 0);
	hbox = gtk_hbox_new(FALSE, 0);
	hboxtwo = gtk_hbox_new(FALSE, 0);
	vscroller = gtk_vscrollbar_new(GTK_TEXT(agreetext)->vadj);
	gtk_signal_connect(GTK_OBJECT(agreebtn), "clicked", 
					   GTK_SIGNAL_FUNC(concurrence), sess);
	gtk_signal_connect(GTK_OBJECT(disagreebtn), "clicked", 
					   GTK_SIGNAL_FUNC(disagreement), sess);
	gtk_container_add(GTK_CONTAINER(agreementwin), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), hboxtwo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hboxtwo), agreetext, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hboxtwo), vscroller, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), agreebtn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), disagreebtn, FALSE, FALSE, 0);
	gtk_text_insert(GTK_TEXT(agreetext),  gtkhx_font, NULL, NULL, agreement, 
					len);
	init_keyaccel(agreementwin);
	gtk_widget_show_all(agreementwin);
	sess->agreementwin = agreementwin;
}

struct output_functions hx_output = {
	init,
	loop,
	hx_clear_chat,
	output_chat,
	msg_output,
	output_agreement,
	output_news_file,
	output_news_post,
	output_user_info,
	output_file_info,
	user_create,
	user_delete,
	user_change,
	user_list,
	users_clear,
	output_file_list,
	file_update,
	tracker_server_create,
	task_update,
	output_news_folder,
	output_news_catalog,
	output_news_thread,
	output_chat_subject,
	output_chat_invitation,
	output_xfer_queue,
	tracker_clear
};


char **hxd_environ = 0;

int hxd_open_max = 0;
struct hxd_file *hxd_files = 0;


void
qbuf_set (struct qbuf *q, guint32 pos, guint32 len)
{
	int need_more = q->pos + q->len < pos + len;

	q->pos = pos;
	q->len = len;
	if (need_more)
		q->buf = g_realloc(q->buf, q->pos + q->len);
}

void
qbuf_add (struct qbuf *q, void *buf, guint32 len)
{
	size_t pos = q->pos + q->len;

	qbuf_set(q, q->pos, q->len + len);
	memcpy(&q->buf[pos], buf, len);
}

extern void hlclient_reap_pid (pid_t pid, int status);

RETSIGTYPE sig_chld (int sig)
{
	int status, serrno = errno;
	pid_t pid;

#ifndef WAIT_ANY
#define WAIT_ANY -1
#endif

	for (;;) {
		pid = waitpid(WAIT_ANY, &status, WNOHANG);
		if (pid < 0) {
			if (errno == EINTR)
				continue;
			goto ret;
		}
		if (!pid)
			goto ret;

		hlclient_reap_pid(pid, status);
	}

  ret:
	errno = serrno;
}

static RETSIGTYPE sig_bus (int sig)
{

	/* do something!! */
	abort();
}


void hotline_client_init (int argc, char **argv);

#if !defined(_SC_OPEN_MAX) && defined(HAVE_GETRLIMIT)
#include <sys/resource.h>
#endif

static RETSIGTYPE
sig_fpe (int sig, int fpe)
{
	g_error("SIGFPE (%d): %d", sig, fpe);
	abort();
}

int
main (int argc, char **argv, char **envp)
{
	struct sigaction act;

	memset(&sessions[0], 0, sizeof(session));

#if defined(_SC_OPEN_MAX)
	hxd_open_max = sysconf(_SC_OPEN_MAX);
#elif defined(RLIMIT_NOFILE)
	{
		struct rlimit rlimit;

		if (getrlimit(RLIMIT_NOFILE, &rlimit)) {
			exit(1);
		}
		hxd_open_max = rlimit.rlim_max;
	}
#elif defined(HAVE_GETDTABLESIZE)
	hxd_open_max = getdtablesize();
#elif defined(OPEN_MAX)
	hxd_open_max = OPEN_MAX;
#else
	hxd_open_max = 16;
#endif
	if (hxd_open_max > FD_SETSIZE)
		hxd_open_max = FD_SETSIZE;
	hxd_files = g_malloc0(hxd_open_max * sizeof(struct hxd_file));

	hxd_environ = envp;

	act.sa_handler = SIG_IGN;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	sigaction(SIGPIPE, &act, 0);
	sigaction(SIGHUP, &act, 0);
	act.sa_handler = (RETSIGTYPE (*)(int))sig_fpe;
	sigaction(SIGFPE, &act, 0);
	act.sa_handler = sig_bus;
	sigaction(SIGBUS, &act, 0);
	act.sa_handler = sig_chld;
	act.sa_flags |= SA_NOCLDSTOP;
	sigaction(SIGCHLD, &act, 0);

	hotline_client_init(argc, argv);

	close(0);
	close(1);

	return 0;
}

char *hxd_commands[] = {
	"0wn",
	"access",
	"away",
	"alert",
	"broadcast",
	"color",
	"exec",
	"g0away",
	"maltbl",
	"mon",
	"users",
	"version",
	"visible",
};

int hxdcmd_len = sizeof(hxd_commands)/sizeof(hxd_commands[0]);

int is_hxdcmd(char *str)
{
	int i;
	char *ptr;

	if(!str) {
		return 0;
	}

	ptr = strchr(str, ' ');

	for(i = 0; i < hxdcmd_len; i++) {
		int _len = strlen(hxd_commands[i]);
		int len = ptr-str>_len?_len:ptr-str;

		if(!strncmp(hxd_commands[i], str, len)) {
			return 1;
		} 
	}

	return 0;
}

void
hotline_client_input (struct htlc_conn *htlc, char *str, guint32 cid, 
					  guint16 style)
{
	if (*str) {
#ifdef USE_PLUGIN
		if(EMIT_SIGNAL(XP_SND_CHAT, sess_from_htlc(htlc), str, &cid, 0, 0, 0)
		   == 1) {
			return;
		}
#endif

		if (*str == '/' && *++str && *str != '/') {
			if(is_hxdcmd(str)) {
				str--;
				hx_send_chat(htlc, str, cid, style);
			}
			else {
				hx_command(str, cid);
			}
		}
		else {
			hx_send_chat(htlc, str, cid, style);
		}
	}
}

void get_password(char *buf)
{
	int i;
	struct termios termio;
	struct termios tmp;

	tcgetattr(0, &termio);
	tmp.c_lflag = termio.c_lflag;
	termio.c_lflag = (ISIG|ICANON);
	tcsetattr(0, TCSANOW, &termio);

	/* The knights who say "nee" demand a..
	   SHRUBBERY! */
	printf("Password: ");
	fgets(buf, 128, stdin);
	printf("\n");
	termio.c_lflag = tmp.c_lflag;
	tcsetattr(0, TCSANOW, &termio);


	for(i = 0; i < strlen(buf); i++) {
		if(buf[i] == '\n') {
			buf[i] = '\0';
			break;
		}
	}
}

void print_help (char *name)
{
	printf(_("GtkHx %s, Copyright (C) 2000-2002 Misha Nasledov\n"), VERSION);
	printf(_("GtkHx comes with ABSOLUTELY NO WARRANTY.\n"));
	printf(_("This is free software, and you are welcome\n"));
	printf(_("to redistribute it under certain conditions.\n\n"));
	
	printf(_("usage: %s [options]\n"), name);
	printf(_("\nsupported options:\n"));
	printf(_("\t--help, -h\t\tPrint this help message out.\n"));
	printf(_("\t--server, -s <host>\tConnect to <host>\n"));
	printf(_("\t--port, -t <port>\tConnect to <host> on <port>. "
			 "(default: 5500)\n"));
	printf(_("\t--login, -l <login>\tUse <login> for <host>. "
			 "(default: guest)\n"));
    printf(_("\t--pass, -p\t\tPrompt for pass of <login>.\n"));
    printf(_("\t--bookmark, -b <name>\tConnect using bookmark <name>.\n"));
}

static struct option hx_options[] = {
	{"server", 1, 0, 's'},
	{"help", 0, 0, 'h'},
	{"port", 1, 0, 't'},
	{"login", 1, 0, 'l'},
	{"pass", 0, 0, 'p'},
	{"bookmark", 1, 0, 'b'},
	{0, 0, 0, 0}
};

extern void connect_bookmark_name(char *name);

void hotline_client_init (int argc, char **argv)
{
	char *home, *user;
	struct passwd *pwe;
	char opt_char;
	int index = 0;
	char *server = 0;
	char *login = 0;
	char *pass = 0;
	char *bookmark = 0;
	int prompt_pass = 0;
	struct opt_r opt;
	guint16 port = 5500;

	opt.ind = 0;
	opt.err_printf = (void (*)(const char *fmt, ...))printf;
	if(argc > 1) {
	while((opt_char = getopt_long_r(argc, argv, "s:ht:pl:b:", hx_options, &index, &opt)) != EOF)  {
		if (opt_char == 0)
			opt_char = hx_options[index].val;
		switch(opt_char) {
		case 't':
			if(opt.arg) {
				port = strtoul(opt.arg, 0, 0);
			}
			break;
		case 's':
			if(opt.arg) {
				server = g_strdup(opt.arg);
				if(bookmark) {
					g_free(bookmark);
				}
			}
			break;
		case 'h':
			print_help(argv[0]);
			exit(0);
			break;
		case 'l':
			if(opt.arg) {
				login = g_strdup(opt.arg);
			}
			break;
		case 'p':
			prompt_pass = 1;
			break;


		case 'b':
			if(opt.arg) {
				bookmark = g_strdup(opt.arg);
				if(server) {
					g_free(server);
				}
				if(login) {
					g_free(login);
				}
			}
			break;

		}

	}
	}
	home = getenv("HOME");
	user = getenv("USER");
	if (!home || !user) {
		pwe = getpwuid(getuid());
		if (!pwe) {
			hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "getpwuid: %s", 
							 strerror(errno));
		} else {
			if (!home)
				home = pwe->pw_dir;
			if (!user)

				user = pwe->pw_name;
		}
	}

	memset(&sessions[0].htlc, 0, sizeof(struct htlc_conn));
	INITLOCK_HTXF((&(sessions[0].htlc)));
	sessions[0].htlc.icon = 500;
	if (user)
	{
		strncpy(sessions[0].htlc.name, user, 31);
		sessions[0].htlc.name[31] = '\0';
	}
	else
		strcpy(sessions[0].htlc.name, "Evaluation 0wn3r");


	gen_command_hash();

	last_msg_nick[0] = 0;

	hx_output.init(argc, argv);

	if(server) {
		if(prompt_pass) {
			pass = g_malloc(128);
			get_password(pass);
		}
		hx_connect(&sessions[0].htlc, server, port, login ? login : "guest", 
				   pass ? pass : "", 0);
		g_free(server);
		g_free(login);
		g_free(pass);
	}
	else if(bookmark) {
		connect_bookmark_name(bookmark);
		g_free(bookmark);
	}

	hx_output.loop();
}
