/*
 * Copyright (C) 2001 Misha Nasledov <misha@nasledov.com>
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
#include <stdlib.h>
#include <stdio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include "hx.h"

pthread_cond_t Xcond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t Xmutex = PTHREAD_MUTEX_INITIALIZER;
int Xpipe[2];

/* this routine is called when something has written to the Xpipe */
static void threadcall(void *a,int b,GdkInputCondition c) 
{
	debug("gtkthreads: call\n");

	pthread_cond_wait(&Xcond,&Xmutex);
	debug("gtkthreads: call 2\n");

}

void gtk_threads_init(void) {
	pipe(Xpipe);
	pthread_mutex_lock(&Xmutex);
}

void gtk_threads_main() {
	gdk_input_add(Xpipe[0],GDK_INPUT_READ,threadcall,0);
	gtk_main();
}


/*
  gtk_calls not made by the main thread should be wrapped between
  gtk_thread_enter and gtk_thread_leave
*/
void gtk_threads_enter(b){
	char c;
	debug("gtkthreads: enter\n");

	write(Xpipe[1],"A",1);
	debug("gtkthreads: enter 2\n");

	pthread_mutex_lock(&Xmutex);
	debug("gtkthreads: enter 3\n");

	read(Xpipe[0],&c,1);

	debug("gtkthreads: enter 4\n");
}

void gtk_threads_leave(){
	debug("gtkthreads: leave\n");
	pthread_cond_signal(&Xcond);
	debug("gtkthreads: leave 2\n");
	pthread_mutex_unlock(&Xmutex);
	debug("gtkthreads: leave 3\n");
}

void gtk_thread_exit(){
	pthread_cond_destroy(&Xcond);
	pthread_mutex_destroy(&Xmutex);
	close(Xpipe[0]);
	close(Xpipe[1]);
}
