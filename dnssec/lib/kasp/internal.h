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

#pragma once

#include "zone.h"

/*!
 * KASP store API implementation.
 */
typedef struct dnssec_kasp_store_functions {
	// internal context initialization
	int (*open)(void **ctx_ptr, const char *config);
	void (*close)(void *ctx);
	// zone serialization/deserialization
	int (*load_zone)(dnssec_kasp_zone_t *zone, void *ctx);
	int (*save_zone)(dnssec_kasp_zone_t *zone, void *ctx);
} dnssec_kasp_store_functions_t;

/*!
 * DNSSEC KASP reference.
 */
struct dnssec_kasp {
	const dnssec_kasp_store_functions_t *functions;
	void *ctx;
};

int dnssec_kasp_create(dnssec_kasp_t **kasp_ptr,
                       const dnssec_kasp_store_functions_t *functions,
                       const char *open_config);
