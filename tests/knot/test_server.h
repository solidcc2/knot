/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#pragma once

#include "test_conf.h"
#include "knot/server/server.h"
#include "knot/zone/adjust.h"
#include "contrib/mempattern.h"

/* Some domain names. */
#define ROOT_DNAME ((const uint8_t *)"")
#define EXAMPLE_DNAME ((const uint8_t *)"\x7""example")
#define IDSERVER_DNAME ((const uint8_t *)"\2""id""\6""server")

/* Create fake root zone. */
static inline void create_root_zone(server_t *server, knot_mm_t *mm)
{
	/* SOA RDATA. */
	#define SOA_RDLEN 30
	static const uint8_t SOA_RDATA[SOA_RDLEN] = {
	        0x02, 'n', 's', 0x00,          /* ns. */
	        0x04, 'm', 'a', 'i', 'l', 0x00,/* mail. */
	        0x77, 0xdf, 0x1e, 0x63,        /* serial */
	        0x00, 0x01, 0x51, 0x80,        /* refresh */
	        0x00, 0x00, 0x1c, 0x20,        /* retry */
	        0x00, 0x0a, 0x8c, 0x00,        /* expire */
	        0x00, 0x00, 0x0e, 0x10         /* min ttl */
	};

	/* Insert root zone. */
	zone_t *root = zone_new(ROOT_DNAME);
	root->server = server;
	root->contents = zone_contents_new(root->name, true);

	knot_rrset_t *soa = knot_rrset_new(root->name, KNOT_RRTYPE_SOA, KNOT_CLASS_IN,
	                                   7200, mm);
	knot_rrset_add_rdata(soa, SOA_RDATA, SOA_RDLEN, mm);
	node_add_rrset(root->contents->apex, soa, NULL);
	knot_rrset_free(soa, mm);

	/* Bake the zone. */
	(void)zone_adjust_full(root->contents, 1);

	/* Switch zone db. */
	knot_zonedb_free(&server->zone_db);
	server->zone_db = knot_zonedb_new();
	knot_zonedb_insert(server->zone_db, root);
}

/* Create fake server. */
static inline int create_fake_server(server_t *server, knot_mm_t *mm, const char *db_storage)
{
	int ret;

	/* Create test configuration. */
	/* String `db_storage' obtained from test_mkdtemp() may be up to 4096 bytes. */
	char conf_str[4096 + 512];
	(void)snprintf(conf_str, sizeof(conf_str),
		"server:\n"
		"    identity: bogus.ns\n"
		"    version: 0.11\n"
		"    nsid: \n"
		"database:\n"
		"    storage: %s\n"
		"zone:\n"
		"  - domain: .\n"
		"    zonefile-sync: -1\n",
		db_storage);

	/* Load test configuration. */
	ret = test_conf(conf_str, NULL);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Create name server. */
	ret = server_init(server, 1);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Insert root zone. */
	create_root_zone(server, mm);

	return KNOT_EOK;
}
