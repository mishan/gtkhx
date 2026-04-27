#ifndef PLUGIN_H

#include <netinet/in.h>

#define MODULE_IFACE_VER	2
#define INFOPREFIX	" \00310[\003\00303hx\003\00310]\003 "

#ifndef MAXPATHLEN
#ifdef PATH_MAX
#define MAXPATHLEN PATH_MAX
#else
#define MAXPATHLEN 4095
#endif
#endif

enum
{ XP_RCV_CHAT = 0, XP_SND_CHAT, XP_RCV_MSG, XP_SND_MSG, XP_RCV_AGREE, XP_RCV_SUBJ, XP_RCV_INVITE, NUM_XP
		
};

#define XP_CALLBACK(x)	( (int (*) (void *, void *, void *, void *, void *, char) ) x )

struct qbuf {
	u_int32_t pos, len;
	u_int8_t *buf;
};

#define	EMIT_SIGNAL(s, a, b, c, d, e, f) (fire_signal(s, a, b, c, d, e, f))
#define XP_CALLNEXT(s, a, b, c, d, e, f) return 0;
#define XP_CALLNEXT_ANDSET(s, a, b, c, d, e, f) return 1;
extern void fe_pluginlist_update (void);
extern int current_signal;
extern int fire_signal (int, void *, void *, void *, void *, void *, char);


struct module
{
	void *handle;
	char *name, *desc;
	struct module *next, *last;
};

struct module_cmd_set
{
	struct module *mod;
	struct commands *cmds;
	struct module_cmd_set *next, *last;
};


struct xp_signal
{
	int signal;
	int (**naddr) (void *, void *, void *, void *, void *, char);
	int (*callback) (void *, void *, void *, void *, void *, char);
	/* These aren't used, but needed to keep compatibility --AGL */
	void *next, *last;
	void *data;
	struct module *mod;
};

struct pevt_stage1
{
	int len;
	char *data;
	struct pevt_stage1 *next;
};

extern int hook_signal (struct xp_signal *sig);
extern void unhook_signal (struct xp_signal *sig);

extern struct module *modules;
extern struct module_cmd_set *mod_cmds;
extern int module_load (char *name);
extern int module_unload (char *name);

struct hl_access_bits {
#if WORDS_BIGENDIAN
	u_int32_t delete_files:1,
		  upload_files:1,
		  download_files:1,
		  rename_files:1,
		  move_files:1,
		  create_folders:1,
		  delete_folders:1,
		  rename_folders:1,
		  move_folders:1,
		  read_chat:1,
		  send_chat:1,
		  __reserved0:3,
		  create_users:1,
		  delete_users:1,
		  read_users:1,
		  modify_users:1,
		  __reserved1:2,
		  read_news:1,
		  post_news:1,
		  disconnect_users:1,
		  cant_be_disconnected:1,
		  get_user_info:1,
		  upload_anywhere:1,
		  use_any_name:1,
		  dont_show_agreement:1,
		  comment_files:1,
		  comment_folders:1,
		  view_drop_boxes:1,
		  make_aliases:1,
		  can_broadcast:1,
		  __reserved2:7,
		  __reserved3:24;
#else /* assumes little endian */
	u_int32_t rename_folders:1,
		  delete_folders:1,
		  create_folders:1,
		  move_files:1,
		  rename_files:1,
		  download_files:1,
		  upload_files:1,
		  delete_files:1,
		  delete_users:1,
		  create_users:1,
		  __reserved0:3,
		  send_chat:1,
		  read_chat:1,
		  move_folders:1,
		  cant_be_disconnected:1,
		  disconnect_users:1,
		  post_news:1,
		  read_news:1,
		  __reserved1:2,
		  modify_users:1,
		  read_users:1,
		  make_aliases:1,
		  view_drop_boxes:1,
		  comment_folders:1,
		  comment_files:1,
		  dont_show_agreement:1,
		  use_any_name:1,
		  upload_anywhere:1,
		  get_user_info:1,
		  __reserved2:7,
		  can_broadcast:1,
		  __reserved3:24;
#endif
};


struct extra_access_bits {
	u_int32_t chat_private:1,
		  msg:1,
		  user_getlist:1,
		  file_list:1,
		  file_getinfo:1,
		  file_hash:1,
		  can_login:1,
		  is_server:1,
		  user_visibility:1,
		  user_color:1,
		  can_spam:1,
		  __reserved:21;
};


struct task {           
        struct task *next, *prev;
        u_int32_t trans;
        u_int32_t pos, len;
        void *data;        
        char *str;
        void *ptr;
        void (*rcv)();
};
struct htlc_conn {
	struct htlc_conn *next, *prev;
	void (*rcv)(struct htlc_conn *);
	void (*real_rcv)(struct htlc_conn *);
	struct qbuf in, out;
#ifdef USE_IPV6
	struct addrinfo *addr;
#else
	struct sockaddr_in addr;
#endif
	int fd;
	u_int32_t trans;
	u_int32_t chattrans;
	u_int32_t icon;
	u_int16_t uid;
	u_int16_t color;
	u_int16_t version;
	struct {
		u_int32_t visible:1, reserved:31;
	} flags; 
	struct hl_access_bits access;
	struct extra_access_bits access_extra;
	u_int16_t current_uid;
	u_int8_t userid[513];
	u_int8_t name[32];
	u_int8_t login[32];
	char rootdir[MAXPATHLEN];
	int gdk_input;
};

typedef struct {
	int xsize, ysize;
	int xpos, ypos;
	int open;
	int init;
} Window_Geo;

struct gtkhx_prefs {
	int queuedl;
	int showjoin;
	int showback;
	int auto_reply;
	int use_fontset;
	int num_tracker;
	int num_icons;
	int trans_xtext;
#ifdef USE_GDK_PIXBUF
	int tint_red, tint_blue, tint_green;
#endif
	char *sound_path;
	char *auto_reply_msg;
	char *font;
	char *download_path;
	char **tracker;
	char *tracker_str;
	char *socks_server;
	char **icon;
	char *icon_str;
	int use_socks;
	int socks_port;
	int timestamp;
	char *snd_cmd;
	int word_wrap;
	int file_samewin;
	int track_case;
	int xbuf_max;

	struct {
		void *options_window;
		void *nick;
		void *icon;
		void *showjoin;
		void *tracker;
		void *showback;
		void *queuedl;
		void *icon_path;
		void *sound_path;
		void *downloadpath;
		void *auto_reply;
		void *auto_reply_msg;
		void *font;
		void *use_fontset;
		void *trans_xtext;
		void *use_socks;
		void *socks_server;
		void *socks_port;
		void *timestamp;
		void *snd_cmd;
		void *word_wrap;
		void *file_samewin;
		void *track_case;
		void *xbuf_max;
	} gtk;

	struct {
		Window_Geo chat;
		Window_Geo news;
		Window_Geo tool;
		Window_Geo tasks;
		Window_Geo users;
	} geo;
};
struct gtkhx_prefs gtkhx_prefs;

struct msgwin {
	struct msgwin *next, *prev;
	u_int32_t uid;
	char *name;
	void *outputbuf;
	void *inputbuf;
	void *vscroll;
	void *window;
};


struct hx_user {
	struct hx_user *next, *prev;
	u_int32_t uid;
	u_int16_t icon;
	u_int16_t color;
	u_int8_t name[32];
	int ignore;
};

struct chat {
	struct chat *next, *prev;
	u_int32_t cid;
	u_int32_t nusers;
	struct hx_user __user_list;
	struct hx_user *user_list;
	struct hx_user *user_tail;
	char subject[256];
};
struct gtkhx_chat {
	struct gtkhx_chat *next, *prev;
	void *window;
	void *vscroll;
	void *output;
	void *input;
	void *subject;
	void *userlist;
	u_int32_t cid;
	struct chat *chat;
	void *chat_history;
};
struct hx_sounds {
	int on;
	int invite, chat, error, file, join, login, msg, news, part;
	struct {
		void *on;
		void *inv;
		void *chat;
		void *err;
		void *file;
		void *join;
		void *login;
		void *msg;
		void *news;
		void *part;
	} gtk;
};

typedef struct _session {
	void *toolbar_window;
	void *news_window;
	void *chat_window;
	void *tasks_window;
	void *users_window;

	void *users_list;

	void *news_text;
	void *postButton;
	void *reloadButton;

	void *agreementwin;

	struct gtkhx_chat *gchat_list;

	struct gtask *gtask_list;
	void *gtklist, *gtask_scroll;

	void *gfnews_list; /* unimplemented */

	void *chat_invite_list; /* unimplemented */

	void *gfile_list; /* unimplemented */

	struct msgwin *msg_list;

	void *gcnews_list; /* unimplemented */

	struct task __task_list;
	struct task *task_list, *task_tail;
	
	struct hx_user __user_list;
	struct hx_user *user_list, *user_tail;
	
	struct chat *chat_front, *chat_tail, *chat_list;
	struct chat __chat_list;

	void *cfl_list; /* unimplemented */

	struct htlc_conn htlc;
} session;

extern session *sess_from_htlc(struct htlc_conn *htlc);

extern void hx_printf_prefix (struct htlc_conn *htlc, u_int32_t cid, const char *prefix, const char *fmt, ...);
extern void hx_clear_chat (struct htlc_conn *htlc, u_int32_t cid);
extern void hx_printf (struct htlc_conn *htlc, u_int32_t cid, const char *fmt, ...);


#endif
