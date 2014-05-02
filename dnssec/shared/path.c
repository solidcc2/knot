/*  Copyright (C) 2014 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "path.h"

char *path_normalize(const char *path)
{
	char real[MAX_PATH] = { '\0' };
	if (!realpath(path, real)) {
		return NULL;
	};

	struct stat st = { 0 };
	if (stat(real, &st) == -1) {
		return NULL;
	}

	if (!S_ISDIR(st.st_mode)) {
		return NULL;
	}

	return strdup(real);
}
