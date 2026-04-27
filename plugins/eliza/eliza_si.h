/*
    This is an implementation of Eliza program for GAIM
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

struct stored_user_info {
	u_int32_t uid;
    char stored_phrase[BUFLENGTH];
    char stored_reply[BUFLENGTH];
    char stored_previous[BUFLENGTH];
    struct stored_user_info* next;
};

typedef struct stored_user_info* si_ptr;

si_ptr si_tree = NULL;

si_ptr find_si(si_ptr si_tree, u_int32_t uid)
{
    if (uid == si_tree->uid)
	return si_tree;
    else
	if (!(si_tree->next))
	    return NULL;
	else 
	    return find_si(si_tree->next, uid);
}


void insert_si(si_ptr *si_old, u_int32_t uid, char *phrasem, char *replym, char *previousm)
{
    si_ptr new_si;
    if (!(new_si = (si_ptr)malloc(sizeof(struct stored_user_info))))
    {
	printf("Out of memory.\n");
	exit(3);
    }

    new_si->uid = uid;
    strncpy(new_si->stored_phrase,phrasem,BUFLENGTH);
    strncpy(new_si->stored_reply,replym,BUFLENGTH);
    strncpy(new_si->stored_previous,previousm,BUFLENGTH);
    new_si->next = *si_old;
    *si_old = new_si;
}

void delete_si(si_ptr si_del)
{
    if (si_del)
	delete_si(si_del->next);
    free(si_del);
}
