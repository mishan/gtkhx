#define USE_PLUGIN

#include <stdio.h>
#include "../plugin_h.h"

extern	struct module *module_find (char *name);

char	*name = "Sample";
char	*desc = "This is a sample module of no use!";

int	sample_rcv_chat ( session *sess, char *text, u_int32_t *cid, u_int32_t *uid, void *b, char c);
int sample_snd_chat ( session *sess, char *text, u_int32_t *cid, void *a, void *b, char c);
int sample_rcv_msg ( session *sess, char *text, char *name, u_int32_t *uid, void *b, char c);
int sample_snd_msg ( session *sess, char *text, u_int32_t *uid, void *a, void *b, char c);
int sample_rcv_agree( session *sess, char *text, int *len, void *a, void *b, char c);
int sample_rcv_subj ( session *sess, char *subject, u_int32_t *cid, void *a, void *b, char c);
int sample_rcv_invite ( session *sess, char *name, u_int32_t *uid, u_int32_t *cid, void *b, char c);

struct	xp_signal	chat_rcv_sig, chat_snd_sig, msg_rcv_sig, msg_snd_sig, agree_rcv_sig, subj_rcv_sig, inv_rcv_sig;

int	module_init (int ver, struct module *mod, session *sess)
{
	/* This check *MUST* be done first */
	if (ver != MODULE_IFACE_VER)
		return 1;
	
	if (module_find (name) != NULL) {
		/* We are already loaded */
		hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, "Module sample already loaded\n");
		return 1;
	}
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, "Loaded module sample\n");
	
	mod->name = name;
	mod->desc = desc;
		
	chat_rcv_sig.signal = XP_RCV_CHAT;
	chat_rcv_sig.callback = XP_CALLBACK(sample_rcv_chat);
	chat_rcv_sig.naddr = 0;
	chat_rcv_sig.mod = mod;
	
	chat_snd_sig.signal = XP_SND_CHAT;
	chat_snd_sig.callback = XP_CALLBACK(sample_snd_chat);
	chat_snd_sig.naddr = 0;
	chat_snd_sig.mod = mod;

	msg_rcv_sig.signal = XP_RCV_MSG;
	msg_rcv_sig.callback = XP_CALLBACK(sample_rcv_msg);
	msg_rcv_sig.naddr = 0;
	msg_rcv_sig.mod = mod;

	msg_snd_sig.signal = XP_SND_MSG;
	msg_snd_sig.callback = XP_CALLBACK(sample_snd_msg);
	msg_snd_sig.naddr = 0;
	msg_snd_sig.mod = mod;

	agree_rcv_sig.signal = XP_RCV_AGREE;
	agree_rcv_sig.callback = XP_CALLBACK(sample_rcv_agree);
	agree_rcv_sig.naddr = 0;
	agree_rcv_sig.mod = mod;

	subj_rcv_sig.signal = XP_RCV_SUBJ;
	subj_rcv_sig.callback = XP_CALLBACK(sample_rcv_subj);
	subj_rcv_sig.naddr = 0;
	subj_rcv_sig.mod = mod;
	
	inv_rcv_sig.signal = XP_RCV_INVITE;
	inv_rcv_sig.callback = XP_CALLBACK(sample_rcv_invite);
	inv_rcv_sig.naddr = 0;
	inv_rcv_sig.mod = mod;

	hook_signal(&chat_rcv_sig);
	hook_signal(&chat_snd_sig);
	hook_signal(&msg_rcv_sig);
	hook_signal(&msg_snd_sig);
	hook_signal(&agree_rcv_sig);
	hook_signal(&subj_rcv_sig);
	hook_signal(&inv_rcv_sig);

	return 0;
}

void	module_cleanup (struct module *mod, session *sess)
{
	hx_printf_prefix(&sess->htlc,0, INFOPREFIX, "Sample module unloading\n");
}

int sample_snd_chat ( session *sess, char *text, u_int32_t *cid, void *a, void *b, char c)
{
	printf("sample: sending \"%s\" to 0x%08x\n", text, *cid);
	fflush(stdout);

	XP_CALLNEXT(0, sess, text, cid, a, b, c);
}

int	sample_rcv_chat ( session *sess, char *text, u_int32_t *cid, u_int32_t *uid, void *b, char c)
{	
	printf("sample: received \"%s\" from %u in 0x%08x\n", text, *uid, *cid);
	fflush(stdout);

	XP_CALLNEXT(0, sess, text, cid, uid, b, c);
}

int sample_rcv_msg (session *sess, char *text, char *name, u_int32_t *uid, void *b, char c)
{
	printf("sample: received msg \"%s\" from \"%s\" [%u]\n", text, name, *uid);
	fflush(stdout);

	/* kick the bastard who messages you
	   a BOFH production ;)

	   {
	   struct hx_user *user = hx_user_with_uid(sess->chat_front->user_list, *uid);
	   hx_kick_user(&sess->htlc, user, 1);
	   }
	*/

	XP_CALLNEXT(0, sess, text, name, uid, b, c);
}

int sample_snd_msg (session *sess, char *text, u_int32_t *uid, void *a, void *b, char c)
{
	printf("sample: sending msg \"%s\" to %u\n", text, *uid);
	fflush(stdout);
	
	XP_CALLNEXT(0, sess, text, uid, a, b, c);
}

int sample_rcv_agree (session *sess, char *text, int *len, void *a, void *b, char c)
{
	printf("sample: received agreement:\n%.*s\n", *len, text);
	fflush(stdout);
	
	/*  ignore the agreement 
		return 1;
	*/

	XP_CALLNEXT(0, sess, text, len, a, b, c);
}

int sample_rcv_subj (session *sess, char *subj, u_int32_t *cid, void *a, void *b, char c)
{
	printf("sample: received subject for 0x%08x: \"%s\"\n", *cid, subj);
	fflush(stdout);

	XP_CALLNEXT(0, sess, text, cid, a, b, c);
}

int sample_rcv_invite (session *sess, char *name, u_int32_t *uid, u_int32_t *cid, void *b, char c)
{
	printf("sample: received chat invite from \"%s\" [%u] to 0x%08x\n", name, *uid, *cid);
	fflush(stdout);
	
	XP_CALLNEXT(0, sess, name, uid, cid, b, c);
}
