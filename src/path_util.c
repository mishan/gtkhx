/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
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
 */

/*
 * Pure path helpers, free of GTK / global-state dependencies. See
 * path_util.h for the rationale (testability).
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "path_util.h"

char *
path_basename (char *path, char sep)
{
	size_t len = strlen (path);

	while (len--) {
		if (path[len] == sep)
			return path + len + 1;
	}

	return path;
}
