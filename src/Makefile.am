bin_PROGRAMS = gtkhx

gtkhx_SOURCES = \
	about.c \
	basename.c \
	chat.c \
	cicn.c \
	cipher.c \
	commands.c \
	compress.c \
	connect.c \
	dfa.c \
	files.c \
	getopt_r.c \
	getopt1_r.c \
	gtk_hlist.c \
	gtkhx.c \
	gtkthreads.c \
	gtkutil.c \
	haval.c \
	hfs.c \
	history.c \
	hmac.c \
	localtime_r.c \
	log.c \
	macres.c \
	md5.c \
	msg.c \
	network.c \
	news.c \
	news15.c \
	options.c \
	plugin.c \
	preview.c \
	rand.c \
	rcv.c \
	snprintf.c \
	sha.c \
	sound.c \
	strcasestr.c \
	tasks.c \
	toolbar.c \
	tracker.c \
	usermod.c \
	users.c \
	xfers.c \
	xtext.c


gtkhx_LDADD = -L/usr/local/lib @INTLLIBS@

INCLUDES = \
	-DLOCALEDIR=\"$(localedir)\" \
	-I/usr/local/include \
	-I. \
	-I.. \
	-I../intl \
	-Wall \
	-D_REENTRANT \
	-D_GNU_SOURCE
