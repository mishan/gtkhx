 /* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

/* plugin.c by Adam Langley */

#include "config.h"

#ifdef USE_PLUGIN

#define PLUGIN_C

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <dlfcn.h>
#include "hx.h"
#include "chat.h"
#include "plugin.h"
#include "gtkutil.h"

void unhook_all_by_mod (struct module *mod);
int module_load (char *name);

struct module *modules;
struct module_cmd_set *mod_cmds;

int current_signal;
GSList *sigroots[NUM_XP];
void *signal_data;
/*int (*sighandler[NUM_XP]) (void *, void *, void *, void *, void *, char);*/
extern void printevent_setup ();

static GtkWidget *plugin_window;
static GtkWidget *plugin_list;
static int plugin_row, plugin_col;

static void unload_plugin (GtkWidget *widget, gpointer data)
{
	char *modname;

	gtk_clist_get_text (GTK_CLIST (plugin_list), plugin_row, 0, &modname);
	module_unload (modname);
}

static void plugin_row_select (GtkWidget *widget, gint row, gint col, GdkEventButton *event, gpointer data)
{
	plugin_row = row; plugin_col = col;
}

static void close_load_plugin (GtkWidget *btn, GtkWidget *window)
{
	gtk_widget_destroy(window);
}

static void do_load(GtkWidget *m, gpointer n) {
	char* file = gtk_file_selection_get_filename(GTK_FILE_SELECTION(n));

	module_load(file);
	gtk_widget_destroy(GTK_WIDGET(n));
}

static void load_plugin (GtkWidget *widget, gpointer data)
{
	GtkWidget *config;


	config = gtk_file_selection_new(_("GtkHx - Select Plugin"));


	gtk_file_selection_hide_fileop_buttons(GTK_FILE_SELECTION(config));


	gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(config)->ok_button),
			"clicked", GTK_SIGNAL_FUNC(do_load), config);


	gtk_signal_connect(GTK_OBJECT(GTK_FILE_SELECTION(config)->cancel_button), "clicked", GTK_SIGNAL_FUNC(close_load_plugin), config);

	init_keyaccel(config);
	gtk_widget_show(config);
}

static void close_plugin_window(GtkWidget *window, gpointer data)
{
	gtk_widget_destroy(plugin_window);
	plugin_window = 0;
	plugin_list = 0;
}

static void fe_pluginlist_update (void)
{
	struct module *mod;
	char *entry[2];

	if (!plugin_list)
		return;

	mod = modules;
	gtk_clist_clear (GTK_CLIST (plugin_list));
	for (;;)
	{
		if (mod == NULL)
			break;
		entry[0] = mod->name;
		entry[1] = mod->desc;
		gtk_clist_append (GTK_CLIST (plugin_list), entry);

		mod = mod->next;
	}
}

void create_plugin_manager(void)
{
	GtkWidget *loadbtn;
	GtkWidget *unloadbtn;
	GtkWidget *closebtn;
	GtkWidget *scroll;
	GtkWidget *btnhbox;
	GtkWidget *vbox;
	char *titles[2];
	
	titles[0] = _("Name");
	titles[1] = _("Description");

	if(plugin_window) {
		return;
	}

	plugin_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(plugin_window), "plugins", "GtkHx");
	gtk_window_set_title(GTK_WINDOW(plugin_window), _("GtkHx - Plugins"));
	gtk_signal_connect(GTK_OBJECT(plugin_window), "delete_event", GTK_SIGNAL_FUNC(close_plugin_window), 0);


	loadbtn = gtk_button_new_with_label(_("Load"));
	gtk_signal_connect(GTK_OBJECT(loadbtn), "clicked", GTK_SIGNAL_FUNC(load_plugin), 0);

	unloadbtn = gtk_button_new_with_label(_("Unload"));

	gtk_signal_connect(GTK_OBJECT(unloadbtn), "clicked", GTK_SIGNAL_FUNC(unload_plugin), 0);

	closebtn = gtk_button_new_with_label(_("Close"));
	gtk_signal_connect(GTK_OBJECT(closebtn), "clicked", GTK_SIGNAL_FUNC(close_plugin_window), 0);


	vbox = gtk_vbox_new(0, 0);
	scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	plugin_list = gtk_clist_new_with_titles(2, titles);
	gtk_clist_set_selection_mode(GTK_CLIST(plugin_list), GTK_SELECTION_EXTENDED);
	gtk_clist_set_column_width (GTK_CLIST (plugin_list), 0, 40);
	gtk_widget_set_usize(plugin_list, 350, 200);
	gtk_signal_connect(GTK_OBJECT(plugin_list), "select_row", GTK_SIGNAL_FUNC(plugin_row_select), 0);
	gtk_container_add(GTK_CONTAINER(scroll), plugin_list);

	gtk_box_pack_start(GTK_BOX(vbox), scroll, 0, 0, 0);

	btnhbox = gtk_hbox_new(0, 0);

	gtk_box_pack_start(GTK_BOX(btnhbox), loadbtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnhbox), unloadbtn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnhbox), closebtn, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(vbox), btnhbox, 0, 0, 0);

	gtk_container_add(GTK_CONTAINER(plugin_window), vbox);

	fe_pluginlist_update();

	init_keyaccel(plugin_window);
	gtk_widget_show_all(plugin_window);

}

int match(const char *mask, const char *string)
{
	register const char *m = mask, *s = string;
	register char ch;
	const char *bm, *bs;		/* Will be reg anyway on a decent CPU/compiler */


	/* Process the "head" of the mask, if any */
	while ((ch = *m++) && (ch != '*')) {
		switch (ch) {
		case '\\':
			if (*m == '?' || *m == '*')
				ch = *m++;
		default:
			if (tolower(*s) != tolower(ch))
				return 0;
		case '?':
			if (!*s++)
				return 0;
		}
	}
	if (!ch)
		return !(*s);


	/* We got a star: quickly find if/where we match the next char */
got_star:
	bm = m;			/* Next try rollback here */
	while ((ch = *m++)) {
		switch (ch) {
		case '?':
			if (!*s++)
				return 0;
		case '*':
			bm = m;
			continue;		/* while */
		case '\\':
			if (*m == '?' || *m == '*')
				ch = *m++;
		default:
			goto break_while;	/* C is structured ? */
		}

	}
break_while:
	if (!ch)
		return 1;			/* mask ends with '*', we got it */
	ch = tolower(ch);
	while (tolower(*s++) != ch) {
		if (!*s)
			return 0;
	}
	bs = s;			/* Next try start from here */

  /* Check the rest of the "chunk" */
  while ((ch = *m++)) {
	  switch (ch) {
	  case '*':
		  goto got_star;
	  case '\\':
		  if (*m == '?' || *m == '*')
			  ch = *m++;
	  default:
		  if (tolower(*s) != tolower(ch)) {
			  m = bm;
			  s = bs;
			  goto got_star;
		  }
	  case '?':
		  if (!*s++)
			  return 0;
	  }
  }
  if (*s) {
	  m = bm;
	  s = bs;
	  goto got_star;
  }
  return 1;
}


int
for_files (char *dirname, char *mask, void callback (char *file))
{
	DIR *dir;
	struct dirent *ent;
	char *buf;
	int i = 0;

	dir = opendir (dirname);
	if (dir) {
		while ((ent = readdir (dir))) {
			if (strcmp (ent->d_name, ".") && strcmp (ent->d_name, "..")) {
				if (match (mask, ent->d_name)) {
					i++;
					buf = g_malloc (strlen (dirname) + strlen (ent->d_name) + 2);
					sprintf (buf, "%s/%s", dirname, ent->d_name);
					callback (buf);
					g_free (buf);
				}
			}
		}
		closedir (dir);
	}
	return i;
}

char *
file_part (char *file)
{
	char *filepart = file;

	if (!file)
		return "";

	while (1) {
		switch (*file) {
		case 0:
			return (filepart);
		case '/':
			filepart = file + 1;
			break;
		}
		file++;
	}
}

static void module_autoload (char *mod)
{
	hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "\002%s\00302: \017%s\00302...\017\n", _("Loading"), file_part (mod));
	module_load (mod);
}

void
signal_setup ()
{
	memset (sigroots, 0, NUM_XP * sizeof (void *));
	/*memset (sighandler, 0, NUM_XP * sizeof (void *)); */
	/* printevent_setup (); */
}

int fire_signal (int s, char *a, char *b, char *c, char *d, char *e, char f)
{
	GSList *cur;
	struct xp_signal *sig;
	int flag = 0;

	/*current_signal = s;
	   if (sighandler[s] == NULL)
	   return 0;
	   return sighandler[s] (a, b, c, d, e, f); */

	cur = sigroots[s];
	current_signal = s;
	while (cur) {
		sig = cur->data;


		signal_data = sig->data;
		if (sig->callback (a, b, c, d, e, f)) {
			flag = 1;
		}
		cur = cur->next;
	}

	return flag;
}

void unhook_signal (struct xp_signal *sig)
{
	/*if (sig->next != NULL)
	   sig->next->last = sig->last;
	   if (sig->last != NULL)
	   {
	   sig->last->next = sig->next;
	   if (sig->next != NULL)
	   *(sig->last->naddr) = sig->next->callback;
	   else
	   *(sig->last->naddr) = NULL;
	   } else
	   {
	   if (sig->next != NULL)
	   sighandler[sig->signal] = sig->next->callback;
	   else
	   sighandler[sig->signal] = NULL;
	   }
	   if (sig == sigroots[sig->signal])
	   sigroots[sig->signal] = NULL;
	 */

	g_assert (sig);
	/*g_assert (sig->signal > 0); */
	/* if sig->signal == -1 then it is an textevent */
	sigroots[sig->signal] = g_slist_remove (sigroots[sig->signal], sig);
}

int
hook_signal (struct xp_signal *sig)
{
	/*if (sig->signal > NUM_XP || sig->signal < 0)
	   return 1;
	   if (sig->naddr == NULL || sig->callback == NULL)
	   return 2;
	   if (sig->mod == NULL)
	   return 3;


	   sig->next = sigroots[sig->signal];
	   if (sigroots[sig->signal] != NULL)
	   {
	   sigroots[sig->signal]->last = sig;
	   *(sig->naddr) = sigroots[sig->signal]->callback;
	   } else
	   *(sig->naddr) = NULL;

	   sig->last = NULL;
	   sighandler[sig->signal] = sig->callback;
	   sigroots[sig->signal] = sig;
	*/


	g_assert (sig);
	g_assert (sig->callback);


	sigroots[sig->signal] = g_slist_prepend (sigroots[sig->signal], sig);


	return 0;
}

void
module_add_cmds (struct module_cmd_set *cmds)
{
	if (mod_cmds != NULL) {
		mod_cmds->last = cmds;
	}
	cmds->next = mod_cmds;
	cmds->last = NULL;
	mod_cmds = cmds;
}

void
module_rm_cmds (struct module_cmd_set *cmds)
{
	if (cmds->last != NULL)
		cmds->last->next = cmds->next;
	if (cmds->next != NULL)
		cmds->next->last = cmds->last;
	if (cmds == mod_cmds)
		mod_cmds = mod_cmds->next;
}

void
module_rm_cmds_mod (void *mod)
{
	struct module_cmd_set *m;
	int found;

	for (;;) {
		found = 0;
		m = mod_cmds;
		for (;;) {
			if (m == NULL)
				break;
			if (m->mod == mod) {
				module_rm_cmds (m);
				found = 1;
				break;
			}
			m = m->next;
		}
		if (found == 0)
			break;
	}
	return;
}

#ifndef RTLD_NOW					  /* OpenBSD lameness */
#ifndef RTLD_LAZY
#define RTLD_NOW 0
#else
#define RTLD_NOW RTLD_LAZY
#endif
#endif

int module_load (char *name)
{
	void *handle;
	struct module *m;
	int (*module_init) (int version, struct module * mod, session *sess);


	handle = dlopen (name, RTLD_NOW);


	if (handle == NULL) {
#ifdef HAVE_DLERROR
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "%s\n", (char *)dlerror());
#else
		hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, "%s%s\n", _("Could not load plugin: "), name);
#endif /* HAVE_DLERROR */
		return 1;
	}

	module_init = dlsym (handle, "module_init");
	if (module_init == NULL)
		return 1;
	m = g_malloc (sizeof (struct module));
	if (module_init (MODULE_IFACE_VER, m, &sessions[0]) != 0) {
		dlclose (handle);
		return 1;
	}
	if (modules != NULL)
		modules->last = m;
	m->handle = handle;
	m->next = modules;
	m->last = NULL;
	modules = m;
	fe_pluginlist_update ();


	return 0;
}

struct module *module_find (char *name)
{
	struct module *mod;

	mod = modules;
	for (;;) {
		if (mod == NULL)
			return NULL;
		if (strcasecmp (name, mod->name) == 0)
			return mod;
		mod = mod->next;
	}
}

int
module_command (char *cmd,char *tbuf, char *word[], char *word_eol[])
{
	/* XXX: Redo this ??? */

/*	struct module_cmd_set *m;
	int i;
	struct commands *cmds;

	m = mod_cmds;

	for (;;)
	{
		if (m == NULL)
			return 1;
		cmds = m->cmds;
		for (i = 0;; i++)
		{
			if (cmds[i].name == NULL)
				break;
			if (strcasecmp (cmd, cmds[i].name) == 0)
			{
				if (cmds[i].needserver && !(connected))
				{
					notc_msg (sess);
					return 0;
				}
				if (cmds[i].needchannel && sess->channel[0] == 0)
				{
					notj_msg (sess);
					return 0;
				}
				if (cmds[i].callback (sess, tbuf, word, word_eol) == TRUE)
					PrintText (sess, cmds[i].help);
				return FALSE;
			}
		}
		m = m->next;
	}
	return TRUE; */
	return 0;
}

void
module_unlink_mod (struct module *mod)
{
	if (mod->last != NULL)
		mod->last->next = mod->next;
	if (mod->next != NULL)
		mod->next->last = mod->last;
	if (mod == modules)
		modules = mod->next;
}

int module_unload (char *name)
{
	struct module *m;
	void (*module_cleanup) (struct module * mod);

	m = modules;
	for (;;)
	{
		if (m == NULL)
			break;
		if (!name || strcasecmp (name, m->name) == 0) {
			module_cleanup = dlsym (m->handle, "module_cleanup");
			if (module_cleanup != NULL)
				module_cleanup (m);
			module_rm_cmds_mod (m);
			unhook_all_by_mod (m);
			dlclose (m->handle);
			module_unlink_mod (m);
			g_free (m);
			if (name) {
				fe_pluginlist_update ();

				return 0;
			}
		}
		m = m->next;
	}
	fe_pluginlist_update ();
	return 0;
}

void
unhook_all_by_mod (struct module *mod)
{
	int c;
	struct xp_signal *sig;

	for (c = 0; c < NUM_XP; c++) {
		if (!sigroots[c])
			continue;
		for (;;) {
			if (!sigroots[c])
				break;
			sig = sigroots[c]->data;
			if (!sig)
				break;
			if (sig->mod == mod) {
				unhook_signal (sig);
				continue;
			}
			break;
		}
	}
}

void module_setup (void)
{

#ifdef WIN32
	char *path = g_strdup_printf("plugins/");
#else
	char *home = getenv("HOME");
	char *path = g_strdup_printf("%s/.hx/plugins/", home);

	if(!home) {
		g_free(path);
		return;
	}
#endif /* WIN32 */

	modules = NULL;
	mod_cmds = NULL;
	for_files (path, "*.so", module_autoload);
	g_free(path);
}
#endif /* USE_PLUGIN */
