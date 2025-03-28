/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <tap/basic.h>

#include "test_conf.h"
#include "libknot/libknot.h"
#include "knot/updates/acl.c"
#include "contrib/sockaddr.h"

#define ZONE	"example.zone"
#define ZONE2	"example2.zone"
#define KEY1	"key1_md5"
#define KEY2	"key2_md5"
#define KEY3	"key3_sha256"

static void check_sockaddr_set(struct sockaddr_storage *ss, int family,
                               const char *straddr, int port)
{
	int ret = sockaddr_set(ss, family, straddr, port);
	ok(ret == 0, "set address '%s'", straddr);
}

void check_update(conf_t *conf, knot_rrset_t *authority, knot_tsig_key_t *key,
                  knot_dname_t *zone_name, bool allowed, const char *desc)
{
	struct sockaddr_storage addr;
	check_sockaddr_set(&addr, AF_INET, "1.2.3.4", 0);

	knot_pkt_t *query = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, NULL);
	assert(query);
	knot_pkt_begin(query, KNOT_AUTHORITY);
	knot_pkt_put(query, 0, authority, 0);

	knot_pkt_t *parsed = knot_pkt_new(query->wire, query->size, NULL);
	ok(knot_pkt_parse(parsed, 0) == KNOT_EOK, "Parse update packet");

	conf_val_t acl = conf_zone_get(conf, C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");

	bool ret = acl_allowed(conf, &acl, ACL_ACTION_UPDATE, &addr, key,
	                       zone_name, parsed, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == allowed, "%s", desc);

	knot_pkt_free(parsed);
	knot_pkt_free(query);
}

static void test_acl_allowed(void)
{
	int ret;
	conf_val_t acl;
	struct sockaddr_storage addr = { 0 };

	knot_dname_t *zone_name = knot_dname_from_str_alloc(ZONE);
	ok(zone_name != NULL, "create zone dname");
	knot_dname_t *zone2_name = knot_dname_from_str_alloc(ZONE2);
	ok(zone2_name != NULL, "create zone2 dname");
	knot_dname_t *key1_name = knot_dname_from_str_alloc(KEY1);
	ok(key1_name != NULL, "create "KEY1);
	knot_dname_t *key2_name = knot_dname_from_str_alloc(KEY2);
	ok(key2_name != NULL, "create "KEY2);
	knot_dname_t *key3_name = knot_dname_from_str_alloc(KEY3);
	ok(key3_name != NULL, "create "KEY3);

	knot_tsig_key_t key0 = { 0 };
	knot_tsig_key_t key1 = { DNSSEC_TSIG_HMAC_MD5,    key1_name };
	knot_tsig_key_t key2 = { DNSSEC_TSIG_HMAC_MD5,    key2_name };
	knot_tsig_key_t key3 = { DNSSEC_TSIG_HMAC_SHA256, key3_name };

	const char *conf_str =
		"key:\n"
		"  - id: "KEY1"\n"
		"    algorithm: hmac-md5\n"
		"    secret: Zm9v\n"
		"  - id: "KEY2"\n"
		"    algorithm: hmac-md5\n"
		"    secret: Zm9v\n"
		"  - id: "KEY3"\n"
		"    algorithm: hmac-sha256\n"
		"    secret: Zm8=\n"
		"\n"
		"remote:\n"
		"  - id: remote_v6_ko\n"
		"    address: [ 2009::1 ]\n"
		"    key: key1_md5\n"
		"  - id: remote_v6_ok\n"
		"    address: [ 127.0.0.1, 2001::1 ]\n"
		"    key: key1_md5\n"
		"    tls: on\n"
		"\n"
		"acl:\n"
		"  - id: acl_key_addr\n"
		"    remote: [ remote_v6_ko, remote_v6_ok ]\n"
		"    action: [ transfer ]\n"
		"  - id: acl_deny\n"
		"    address: [ 240.0.0.2 ]\n"
		"    action: [ notify ]\n"
		"    deny: on\n"
		"  - id: acl_no_action_deny\n"
		"    address: [ 240.0.0.3 ]\n"
		"    deny: on\n"
		"  - id: acl_multi_addr\n"
		"    address: [ 192.168.1.1, 240.0.0.0/24 ]\n"
		"    action: [ notify, update ]\n"
		"    protocol: [ udp, tls ]\n"
		"  - id: acl_multi_key\n"
		"    key: [ key2_md5, key3_sha256 ]\n"
		"    action: [ notify, update ]\n"
		"  - id: acl_range_addr\n"
		"    address: [ 100.0.0.0-100.0.0.5, ::0-::5 ]\n"
		"    action: [ transfer ]\n"
		"  - id: acl_deny_no_action_no_key\n"
		"    address: [ 240.0.0.4 ]\n"
		"    deny: on\n"
		"  - id: acl_notify_key\n"
		"    address: [ 240.0.0.0/24 ]\n"
		"    key: "KEY1"\n"
		"    action: [ notify ]\n"
		"  - id: acl_update_key\n"
		"    key: "KEY1"\n"
		"    update-owner: key\n"
		"    update-type: [ AAAA, A ]\n"
		"    action: [ update ]\n"
		"  - id: acl_update_name\n"
		"    key: "KEY2"\n"
		"    update-owner: name\n"
		"    update-owner-name: [ a, b."KEY2". ]\n"
		"    update-owner-match: equal\n"
		"    action: [ update ]\n"
		"\n"
		"zone:\n"
		"  - domain: "ZONE"\n"
		"    acl: [ acl_key_addr, acl_deny, acl_no_action_deny ]\n"
		"    acl: [ acl_multi_addr, acl_multi_key ]\n"
		"    acl: [ acl_range_addr ]\n"
		"  - domain: "ZONE2"\n"
		"    acl: [ acl_deny_no_action_no_key, acl_notify_key ]\n"
		"  - domain: "KEY1"\n"
		"    acl: acl_update_key\n"
		"  - domain: "KEY2"\n"
		"    acl: acl_update_name";

	ret = test_conf(conf_str, NULL);
	is_int(KNOT_EOK, ret, "Prepare configuration");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_QUERY, &addr, &key1, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TLS);
	ok(ret == true, "Address, key, empty action");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key1, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TLS);
	ok(ret == true, "Address, key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::2", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key1, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Address not match, key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Address match, no key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key2, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Address match, key not match, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "2001::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key1, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Address, key match, action not match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_UDP);
	ok(ret == true, "Second address match, no key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key1, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Second address match, extra key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.2", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Denied address match, no key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.2", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_UPDATE, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TLS);
	ok(ret == true, "Denied address match, no key, action not match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.3", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_UPDATE, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Denied address match, no key, no action");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "1.1.1.1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_UPDATE, &addr, &key3, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == true, "Arbitrary address, second key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET, "100.0.0.1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == true, "IPv4 address from range, no key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone_name);
	ok(acl.code == KNOT_EOK, "Get zone ACL");
	check_sockaddr_set(&addr, AF_INET6, "::1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_TRANSFER, &addr, &key0, zone_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == true, "IPv6 address from range, no key, action match");

	acl = conf_zone_get(conf(), C_ACL, zone2_name);
	ok(acl.code == KNOT_EOK, "Get zone2 ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.4", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key1, zone2_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == false, "Address, key, action, denied");

	acl = conf_zone_get(conf(), C_ACL, zone2_name);
	ok(acl.code == KNOT_EOK, "Get zone2 ACL");
	check_sockaddr_set(&addr, AF_INET, "240.0.0.1", 0);
	ret = acl_allowed(conf(), &acl, ACL_ACTION_NOTIFY, &addr, &key1, zone2_name, NULL, NULL, KNOTD_QUERY_PROTO_TCP);
	ok(ret == true, "Address, key, action, match");

	knot_rrset_t A;
	knot_rrset_init(&A, key1_name, KNOT_RRTYPE_A, KNOT_CLASS_IN, 3600);
	knot_rrset_add_rdata(&A, (uint8_t *)"\x00\x00\x00\x00", 4, NULL);
	check_update(conf(), &A, &key1, key1_name, true, "Update, tsig, type");

	check_update(conf(), &A, &key2, key2_name, false, "Update, tsig, bad name");
	knot_rdataset_clear(&A.rrs, NULL);

	knot_rrset_t MX;
	knot_rrset_init(&MX, key1_name, KNOT_RRTYPE_MX, KNOT_CLASS_IN, 3600);
	knot_rrset_add_rdata(&MX, (uint8_t *)"\x00\x00\x00", 3, NULL);
	check_update(conf(), &MX, &key1, key1_name, false, "Update, tsig, bad type");
	knot_rdataset_clear(&MX.rrs, NULL);

	knot_rrset_t aA;
	knot_dname_t *a_key2_name = knot_dname_from_str_alloc("a."KEY2".");
	ok(a_key2_name != NULL, "create a."KEY2".");
	knot_rrset_init(&aA, a_key2_name, KNOT_RRTYPE_A, KNOT_CLASS_IN, 3600);
	knot_rrset_add_rdata(&aA, (uint8_t *)"\x00\x00\x00\x00", 4, NULL);
	check_update(conf(), &aA, &key2, key2_name, true, "Update, tsig, relative name");
	knot_dname_free(a_key2_name, NULL);
	knot_rdataset_clear(&aA.rrs, NULL);

	knot_rrset_t bA;
	knot_dname_t *b_key2_name = knot_dname_from_str_alloc("b."KEY2".");
	ok(b_key2_name != NULL, "create b."KEY2".");
	knot_rrset_init(&bA, b_key2_name, KNOT_RRTYPE_A, KNOT_CLASS_IN, 3600);
	knot_rrset_add_rdata(&bA, (uint8_t *)"\x00\x00\x00\x00", 4, NULL);
	check_update(conf(), &bA, &key2, key2_name, true, "Update, tsig, absolute name");
	knot_dname_free(b_key2_name, NULL);
	knot_rdataset_clear(&bA.rrs, NULL);

	knot_rrset_t aaA;
	knot_dname_t *aa_key2_name = knot_dname_from_str_alloc("a.a."KEY2);
	ok(aa_key2_name != NULL, "create a.a."KEY2);
	knot_rrset_init(&aaA, aa_key2_name, KNOT_RRTYPE_A, KNOT_CLASS_IN, 3600);
	knot_rrset_add_rdata(&aaA, (uint8_t *)"\x00\x00\x00\x00", 4, NULL);
	check_update(conf(), &aaA, &key2, key2_name, false, "Update, tsig, bad name");
	knot_dname_free(aa_key2_name, NULL);
	knot_rdataset_clear(&aaA.rrs, NULL);

	conf_free(conf());
	knot_dname_free(zone_name, NULL);
	knot_dname_free(zone2_name, NULL);
	knot_dname_free(key1_name, NULL);
	knot_dname_free(key2_name, NULL);
	knot_dname_free(key3_name, NULL);
}

void check_pattern(const char *name_str, const char *pattern_str, bool match)
{
	knot_dname_t *name = knot_dname_from_str_alloc(name_str);
	knot_dname_t *pattern = knot_dname_from_str_alloc(pattern_str);

	ok(match_pattern(name, pattern) == match, "'%s' %s '%s'",
	   name_str, match ? "matched" : "not matched by", pattern_str);

	knot_dname_free(name, NULL);
	knot_dname_free(pattern, NULL);
}

static void test_match_pattern(void)
{
	check_pattern(".", "*", false);
	check_pattern("a", "a", true);
	check_pattern("a", "*", true);
	check_pattern("*", "*", true);
	check_pattern("a", "aa", false);
	check_pattern("aa", "a", false);
	check_pattern("a.b", "*", false);
	check_pattern("a.b", "*.*", true);
	check_pattern("a.b", "a.b", true);
	check_pattern("a.b", "*.*b", false);
	check_pattern("a.b", "*.*.*", false);
	check_pattern("abc", "*", true);
	check_pattern("a.bc.*", "a.*.*", true);
}

int main(int argc, char *argv[])
{
	plan_lazy();

	diag("acl_allowed");
	test_acl_allowed();

	diag("match_pattern");
	test_match_pattern();

	return 0;
}
