#ifndef PLUGIN_H
#define PLUGIN_H
#ifdef USE_PLUGIN

#define MODULE_IFACE_VER	2
#define XP_CALLBACK(x)	( (int (*) (void *, void *, void *, void *, void *, char) ) x )

enum
{ XP_RCV_CHAT = 0, XP_SND_CHAT, XP_RCV_MSG, XP_SND_MSG, XP_RCV_AGREE, XP_RCV_SUBJ, XP_RCV_INVITE, NUM_XP


};

#define	EMIT_SIGNAL(s, a, b, c, d, e, f) (fire_signal(s, a, b, c, d, e, f))
#define XP_CALLNEXT(s, a, b, c, d, e, f) return 0;
#define XP_CALLNEXT_ANDSET(s, a, b, c, d, e, f) return 1;
extern int current_signal;

#ifndef PLUGIN_C
extern int fire_signal (int, void *, void *, void *, void *, void *, char);
#endif
void create_plugin_manager(void);
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

extern struct module *modules;
extern struct module_cmd_set *mod_cmds;
extern int module_load (char *name);
extern int module_unload (char *name);

#endif /* USE_PLUGIN */
#endif /* PLUGIN_H */
