#include "config.h"
#ifndef HAVE_BASENAME
#include <sys/types.h>

char *basename (char *path)
{
	size_t len = strlen(path);
	while (len--) {
		if (path[len] == '/')
			return path+len+1;
	}

	return path;
}
#endif
