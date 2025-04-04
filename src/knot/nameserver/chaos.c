/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include <strings.h>
#include <stdlib.h>

#include "knot/nameserver/chaos.h"
#include "knot/conf/conf.h"
#include "libknot/libknot.h"

#define WISH "Knot DNS developers wish you "
#define HOPE "Knot DNS developers hope you "

static const char *wishes[] = {
	HOPE "have all your important life questions answered without SERVFAIL.",
	WISH "many wonderful people in your domain.",
	WISH "non-empty lymph nodes.",
	HOPE "resolve the . of your problems.",
	WISH "long enough TTL.",
	HOPE "become authoritative master in your domain.",
	HOPE "always find useful PTR in CHAOS.",
	"Canonical name is known to both DNS experts and Ubuntu users.",
	HOPE "never forget both your name and address.",
	"Don't fix broken CNAME chains with glue!",
	WISH "no Additional section in your TODO list.",
	HOPE "won't find surprising news in today's journal.",
	HOPE "perform rollover often just when playing roulette.",
	HOPE "get notified before your domain registration expires.",
};

#undef WISH
#undef HOPE

static const char *get_txt_response_string(knot_pkt_t *response)
{
	char qname[32];
	if (knot_dname_to_str(qname, knot_pkt_qname(response), sizeof(qname)) == NULL) {
		return NULL;
	}

	const char *response_str = NULL;

	/* Allow hostname.bind. for compatibility. */
	if (strcasecmp("id.server.",     qname) == 0 ||
	    strcasecmp("hostname.bind.", qname) == 0) {
		response_str = conf()->cache.srv_ident;
	/* Allow version.bind. for compatibility. */
	} else if (strcasecmp("version.server.", qname) == 0 ||
	           strcasecmp("version.bind.",   qname) == 0) {
		response_str = conf()->cache.srv_version;
	} else if (strcasecmp("fortune.", qname) == 0) {
		if (!conf()->cache.srv_has_version) {
			uint16_t wishno = knot_wire_get_id(response->wire) %
			                  (sizeof(wishes) / sizeof(wishes[0]));
			response_str = wishes[wishno];
		}
	}

	return response_str;
}

static int create_txt_rrset(knot_rrset_t *rrset, const knot_dname_t *owner,
                            const char *response_str, knot_mm_t *mm)
{
	/* Truncate response to one TXT label. */
	size_t response_len = strlen(response_str);
	if (response_len > UINT8_MAX) {
		response_len = UINT8_MAX;
	}

	knot_dname_t *rowner = knot_dname_copy(owner, mm);
	if (rowner == NULL) {
		return KNOT_ENOMEM;
	}

	knot_rrset_init(rrset, rowner, KNOT_RRTYPE_TXT, KNOT_CLASS_CH, 0);
	uint8_t rdata[response_len + 1];

	rdata[0] = response_len;
	memcpy(&rdata[1], response_str, response_len);

	int ret = knot_rrset_add_rdata(rrset, rdata, response_len + 1, mm);
	if (ret != KNOT_EOK) {
		knot_dname_free(rrset->owner, mm);
		return ret;
	}

	return KNOT_EOK;
}

static int answer_txt(knot_pkt_t *response)
{
	const char *response_str = get_txt_response_string(response);
	if (response_str == NULL || response_str[0] == '\0') {
		return KNOT_RCODE_REFUSED;
	}

	knot_rrset_t rrset;
	int ret = create_txt_rrset(&rrset, knot_pkt_qname(response),
	                           response_str, &response->mm);
	if (ret != KNOT_EOK) {
		return KNOT_RCODE_SERVFAIL;
	}

	int result = knot_pkt_put(response, 0, &rrset, KNOT_PF_FREE);
	if (result != KNOT_EOK) {
		knot_rrset_clear(&rrset, &response->mm);
		return KNOT_RCODE_SERVFAIL;
	}

	return KNOT_RCODE_NOERROR;
}

int knot_chaos_answer(knot_pkt_t *pkt)
{
	if (knot_pkt_qtype(pkt) != KNOT_RRTYPE_TXT) {
		return KNOT_RCODE_REFUSED;
	}

	return answer_txt(pkt);
}
