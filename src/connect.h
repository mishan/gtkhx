#ifndef HX_CONNECT_H
#define HX_CONNECT_H

extern void connect_set_entries (const char *address, const char *login, const char *pasword, guint16 port);
extern void set_the_entries(char *address, char *login, char *password, char *port, char secure, char compress, char cipher);
#ifdef USE_GETOPT
extern void connect_bookmark_name(char *name);
#endif
extern void create_connect_window (GtkWidget *btn, gpointer data);

#ifdef CONFIG_COMPRESS
extern int valid_compress (const char *compressalg);
extern char **valid_compressors;
#endif
#ifdef CONFIG_CIPHER
extern int valid_cipher (const char *cipheralg);
extern char **valid_ciphers;
#endif
extern guint8 *list_n (guint8 *list, guint16 listlen, unsigned int n);


#endif
