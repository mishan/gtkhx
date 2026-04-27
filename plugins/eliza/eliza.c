/*
    This is an implementation of Eliza program for GtkHx
    Copyright (C) 2000  Vlad Zbarskiy

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include "../plugin_h.h"
#include "config.h"
#include <sys/types.h>


#define REPLY_SIZE 2048
#define MAXKEY      500
#define MAXREPLY    1000
#define PRONOUNS 32
#define BUFLENGTH 1024
extern	struct module *module_find (char *name);

char *name = "Eliza";
char *desc =  "This is an implementation of Eliza program for GtkHx.";

char *keyword[MAXKEY], *reply[MAXREPLY];
int reply_count[MAXREPLY], reply_index[MAXKEY], replies[MAXKEY];
int kindex, r_index;
char rtb[BUFLENGTH] = "";

struct conversation {
	u_int32_t uid;
	struct conversation *next, *prev;
};

char init_eliza  = 0;

struct conversation *cnv_list;

#include "eliza_si.h"

struct conversation *new_conversation(u_int32_t uid)
{
	struct conversation *cnv = malloc(sizeof(struct conversation));
	
	cnv->uid = uid;
	cnv->prev = cnv_list;
	if(cnv_list)
		cnv_list->next = cnv;
	cnv_list = cnv;

	return cnv;
}

struct conversation *find_conversation(u_int32_t uid)
{
	struct conversation *cnv;

	for(cnv = cnv_list; cnv; cnv = cnv->prev ){
		if(cnv->uid == uid) {
			return cnv;
		}
	}

	return 0;
}

void strr(char* phrase, char* a, char* b)
//replaces all entries of a with b in phrase
{
    if (strlen(a) != strlen(b))
	return;
    while (strstr(phrase,a))
	memcpy(strstr(phrase,a),b,strlen(b));
}

void trimspc(char *initial, char *final)
{
    char buf[BUFLENGTH] = "";
    char tempchar[10];
    int was_space = 0;

    while (*initial)
    {
	if (!was_space)
	{
	    sprintf(tempchar,"%c",*initial);
	    strcat(buf,tempchar);
	    was_space = (!strcmp(tempchar," "));
	}
	else
	{
	    sprintf(tempchar,"%c",*initial);
	    if (strcmp(tempchar," "))
	    {
		was_space=0;
		strcat(buf,tempchar);
	    }
	}
	initial++;
    }
    strcpy(final,buf);
}	    	

void  *alloc(size_t size)
{
	void *p;

	if ((p = (char *)malloc(size)) == NULL) {
		printf("Out of memory.\r\n");
		exit(3);
	}
	return p;
}

void eliza_init(session *sess)	/* Initialize global variables by reading in 
								   eliza.txt */
{
	FILE *fp;
	char buf[BUFLENGTH], filepath[BUFLENGTH];
	int rcount, kcount, more, i, j, between;

	sprintf(filepath, PREFIX "share/gtkhx/plugins/eliza.txt");
	
	if ( (fp=fopen(filepath,"r"))==NULL ) {
		if( (fp = fopen("eliza.txt", "r")) == NULL ) {
			printf("I have encountered an error reading my notes from eliza.txt\n");
			return;
		}
	}
	
	kindex=r_index=kcount=0;
	more = 1;
	while (more) {				/* read keywords */
		between = 1;
		while (fgets(buf,BUFLENGTH-1,fp)) {
			/* If CR, read replies */
			if (buf[0]=='\n' || buf[0]=='\r') {
				if (between)
					continue;
				else
					break;
			}
			between = 0;
			/* allocate memory for keyword string */
			keyword[kindex] = alloc(strlen(buf)+1);
			i=j=0;
			
			//replace smileys with hj and hfj, frownies with qz and qfz
			strr(buf,":)","hj");
			strr(buf,";)","hj");
			strr(buf,":-)","hfj");
			strr(buf,";-)","hfj");

			strr(buf,":(","qz");
			strr(buf,";(","qz");
			strr(buf,":-(","qfz");
			strr(buf,";-(","qfz");			
			strr(buf,":-|","qfz");			
			strr(buf,";-|","qfz");			
			
			while ((keyword[kindex][i]=toupper(buf[j]))) {	/* copy keyword */
				if (buf[j]=='_')		/* replace underscores with spaces */
					keyword[kindex][i] = ' ';
				/* Cut punctuation ect. */
				if (isalnum(buf[j]) || buf[j]==' ' || buf[j]=='_')
					i++;
				j++;
			}
			reply_index[kindex] = r_index;
			kindex++;
			kcount++;
		}
		rcount=0;
		between = 1;
		while ((more = (fgets(buf,BUFLENGTH-1,fp)!=NULL))) {		/* read replies */
			if (buf[0]=='\n' || buf[0]=='\r') {
				if (between)
					continue;
				else
					break;
			}
			between = 0;
		
			reply[r_index] = alloc(strlen(buf)+1);
			i=j=0;

			strr(buf,":)","hj");
			strr(buf,";)","hj");
			strr(buf,":-)","hfj");
			strr(buf,";-)","hfj");

			strr(buf,":(","qz");
			strr(buf,";(","qz");
			strr(buf,":-(","qfz");
			strr(buf,";-(","qfz");			
			strr(buf,":-|","qfz");			
			strr(buf,";-|","qfz");			
	
			while ((reply[r_index][i]=toupper(buf[j]))) {
				if (buf[j]!='\n' && buf[j]!='\r')
					i++;
				j++;
			}
			r_index++;
			rcount++;
		}
        /* update indexes for all keywords in this set */
		while (kcount) {
			replies[kindex-kcount] = rcount;
			reply_count[kindex-kcount] = 0;
			kcount--;
		}
	}
	fclose(fp);
	init_eliza = 1;
}


void change_person(char *s)
{
	static char *pronoun[PRONOUNS] = {
		" I AM "," YOU ARE ",
		" YOU ARE "," I AM ",
		" YA ARE "," I AM ",
		" U ARE "," I AM ",
		" U R "," I AM ",		
		" OF YOU "," OF ME ",
		" ARE "," AM ",
		" R "," AM ",		
		" WERE "," WAS ",
		" YOU "," I ",
		" U "," I ",
		" YA "," I ",
		" YOUR"," MY",
		" IVE "," YOUVE ",
		" IM "," YOURE ",
		" ME "," YOU "
	};
	char buf[BUFLENGTH],*c,*t;
	int i;

	t=s;
	c=buf;
	while ((*c=*t)) {
		for (i=0; i<PRONOUNS; i++)
			if (!strncmp(t,pronoun[i],strlen(pronoun[i]))) {
				strcpy(c,pronoun[i^1]);
				c += strlen(pronoun[i^1]) - 1;
				t += strlen(pronoun[i]) - 1;
				break;
			}
		t++;
		c++;
	}
	strcpy(s,buf);
}

void trunc_comma(char *s)
{
	do {
		if (*s=='.' || !strncmp(s," BUT ",5) )
			break;
	} while (*++s);
	*s='\0';
}

int findkey(char **s, u_int32_t uid)
{
	int key;
	char *c,*d,buf[BUFLENGTH],temp_phrase[BUFLENGTH];
	si_ptr si = find_si(si_tree, uid);

	
	c=buf;
	d=*s;
	while ((*c = toupper(*d))) {
		if (isspace(*c))
			*c = ' ';

		if (*c==',' || *c=='.' || *c=='?' || *c=='!' || *c==';' || *c==':' || *c=='(' || *c==')' ) {
			*c++ = ' ';
			*c++ = '.';
			*c = ' ';
		}
		if (isalnum(*c) || *c==' ')
			c++;
		d++;
	}
	strcpy(*s,buf);
	for (key=0; key<kindex-1; key++)
		if ((c=strstr(*s, keyword[key]))) {
			*s = c;
			break;
		}
	if (key < kindex-1) {
		*s = c + strlen(keyword[key]) -1;
		while (!isspace(**s) && **s)
			(*s)++;
		while (isspace(*(*s+1)))
			(*s)++;
	} else
		if ((c=strstr(*s," MY "))) {
			*s = c + 3;
			strcpy(si->stored_phrase,*s);
			trunc_comma(si->stored_phrase);
			change_person(si->stored_phrase);

			trimspc(si->stored_phrase,temp_phrase);
			strcpy(si->stored_phrase,temp_phrase);
			key = kindex;
		}
	return key;
}

void respond(int key,char *s,char *response, u_int32_t uid)
{
    int r,i;
    char *c, temp_response[BUFLENGTH];

    /* If built-in "MY" was used, just reprint and return */
	if (key==kindex) {
		sprintf(response,"YOUR %s\n",s);
		trimspc(response,temp_response);
		strcpy(response,temp_response);
		return;
	}
    /* If no keyword was found, choose between using the remembered phrase and
        one of the generic no-key responses. */
	if (key==kindex-1 && rand()%5==0 && (find_si(si_tree,uid))->stored_phrase[0]) {
		switch (r=rand()%2) {
			case 0:
				sprintf(response,"DOES THIS HAVE ANYTHING TO DO WITH THE FACT THAT YOUR");
				break;
			case 1:
				sprintf(response,"EARLIER YOU SAID YOUR");
		}
        sprintf(rtb," %s%c\n", (find_si(si_tree,uid))->stored_phrase, r?'.':'?');
	strcat(response,rtb);
        /*reply_count[key]=0;*/
	    trimspc(response,temp_response);
	    strcpy(response,temp_response);
	    return;
	}
    /* Pick one of the replies and display it. */
    i = reply_index[key];
    r = i + reply_count[i];
	c=reply[r];
	while (*c) {
		switch (*c) {
			case '*':
				sprintf(rtb,"%s%s",*s==' '?"":" ",s);
				strcat(response,rtb);
				break;
			default:
				sprintf(rtb,"%c",*c);
				strcat(response,rtb);
		}
		c++;
	}
	sprintf(rtb,"\n");
	strcat(response,rtb);
	trimspc(response,temp_response);
	strcpy(response,temp_response);

    reply_count[i]++;
    if (reply_count[i]==replies[key])
        reply_count[i] = 0;
	if (reply_index[key]==reply_index[kindex-2])
		return;
}

int im_reply(session *sess, char *text, char *name, u_int32_t *uid, void *a, char b) {
	char tmpbuf[BUFLENGTH-2],buf[BUFLENGTH] = " ",*s,temp_s[BUFLENGTH];
	int key,to_continue=1, dupl=0, count;
	char reply_msg[REPLY_SIZE] = "";
	struct conversation *cnv;
	si_ptr some1;
	
	if(!gtkhx_prefs.auto_reply || !init_eliza) {
		return 0;
	}

	if (*uid == sess->htlc.uid)
		return 0;

	cnv = find_conversation(*uid);
	if (!cnv) {
	    cnv = new_conversation(*uid);
	}    

	msg_output(name, *uid, text);
	
	//set the reply_msg here	
	strncpy(tmpbuf, text, BUFLENGTH-2);
	
	
	if (tmpbuf[1]=='\0') 
	    to_continue=0;
	
	
	strr(tmpbuf,":)","hj");
	strr(tmpbuf,";)","hj");
	strr(tmpbuf,":-)","hfj");
	strr(tmpbuf,";-)","hfj");
	strr(tmpbuf,":(","qz");
	strr(tmpbuf,";(","qz");
	strr(tmpbuf,":-(","qfz");
	strr(tmpbuf,";-(","qfz");			
	strr(tmpbuf,":-|","qfz");			
	strr(tmpbuf,";-|","qfz");			
	
	for(count=0;tmpbuf[count]!=0;count++)
	    tmpbuf[count]=toupper(tmpbuf[count]);
	
	if (si_tree) {
	    some1 = find_si(si_tree,*uid);	
	    if (some1)
			{
		    if (!strcmp(tmpbuf,some1->stored_previous)) {
			sprintf(reply_msg,"Please don't repeat yourself!\n");
			    dupl = 1;
		    }
		    else
	    		strncpy(some1->stored_previous,tmpbuf,BUFLENGTH);
		}
	    else
		insert_si(&si_tree, *uid, "", "", tmpbuf);
	} 
	else 
	    insert_si(&si_tree, *uid, "", "", tmpbuf);
	
	if (!strcmp(tmpbuf,"TAKE ME OFF")) {
	    to_continue=0;
	    dupl=1;
	}		
	
	strcat(buf,tmpbuf);
	strcat(buf," ");
	s = buf;
	key = findkey(&s,*uid);
	
	trunc_comma(s);
	change_person(s);

	trimspc(s,temp_s);
	strncpy(s,temp_s,BUFLENGTH);

	if (!dupl)
		respond(key,s,reply_msg,*uid);
	
	if (to_continue) {

		
		strr(reply_msg, "HJ", ":)");
		strr(reply_msg, "HFJ", ":-)");		
		strr(reply_msg, "QZ", ":(");
		strr(reply_msg, "QFZ", ":-(");
		
		for(count=0;reply_msg[count]!=0;count++)
		    reply_msg[count]=tolower(reply_msg[count]);

		hx_send_msg(&sess->htlc, *uid, reply_msg, strlen(reply_msg), 0);
		msg_output(sess->htlc.name, *uid, reply_msg);
		strncpy((find_si(si_tree, *uid))->stored_reply,reply_msg,BUFLENGTH);
	}
	
	return 1; /* don't bother handling this message further */
}

struct xp_signal msg_rcv;

int module_init (int ver, struct module *mod, session *sess)
{
	if (ver != MODULE_IFACE_VER)
		return 1;
	
	if (module_find (name) != NULL) {
		/* We are already loaded */
		hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, "Module 'Eliza' already loaded\n");
		return 1;
	}
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, "Loaded module 'Eliza'\n");
	
	mod->name = name;
	mod->desc = desc;
	
	eliza_init(sess);

	msg_rcv.signal = XP_RCV_MSG;
	msg_rcv.callback = XP_CALLBACK(im_reply);
	msg_rcv.naddr = 0;
	msg_rcv.mod = mod;

	hook_signal(&msg_rcv);

	return 0;
}

void module_cleanup (struct module *mod, session *sess)
{
    delete_si(si_tree);
	hx_printf_prefix(&sess->htlc, 0, INFOPREFIX, "Module 'Eliza' unloaded\n");
}
