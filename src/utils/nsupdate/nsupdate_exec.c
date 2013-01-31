/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils/nsupdate/nsupdate_exec.h"
#include "utils/common/params.h"
#include "utils/common/msg.h"
#include "utils/common/exec.h"
#include "utils/common/netio.h"
#include "utils/common/token.h"
#include "common/errcode.h"
#include "common/mempattern.h"
#include "libknot/dname.h"
#include "libknot/util/descriptor.h"
#include "libknot/packet/response.h"
#include "libknot/util/debug.h"
#include "libknot/consts.h"
#include "libknot/packet/query.h"
#include "libknot/tsig-op.h"

/* Declarations of cmd parse functions. */
typedef int (*cmd_handle_f)(const char *lp, params_t *params);
int cmd_add(const char* lp, params_t *params);
int cmd_answer(const char* lp, params_t *params);
int cmd_class(const char* lp, params_t *params);
int cmd_debug(const char* lp, params_t *params);
int cmd_del(const char* lp, params_t *params);
int cmd_gsstsig(const char* lp, params_t *params);
int cmd_key(const char* lp, params_t *params);
int cmd_local(const char* lp, params_t *params);
int cmd_oldgsstsig(const char* lp, params_t *params);
int cmd_prereq(const char* lp, params_t *params);
int cmd_realm(const char* lp, params_t *params);
int cmd_send(const char* lp, params_t *params);
int cmd_server(const char* lp, params_t *params);
int cmd_show(const char* lp, params_t *params);
int cmd_ttl(const char* lp, params_t *params);
int cmd_update(const char* lp, params_t *params);
int cmd_zone(const char* lp, params_t *params);

/* Sorted list of commands.
 * This way we could identify command byte-per-byte and
 * cancel early if the next is lexicographically greater.
 */
const char* cmd_array[] = {
	"\x3" "add",
	"\x6" "answer",
	"\x5" "class",         /* {classname} */
	"\x5" "debug",
	"\x3" "del",
	"\x6" "delete",
	"\x7" "gsstsig",
	"\x3" "key",           /* {name} {secret} */
	"\x5" "local",         /* {address} [port] */
	"\xa" "oldgsstsig",
	"\x6" "prereq",        /* (nx|yx)(domain|rrset) {domain-name} ... */
	"\x5" "realm",         /* {[realm_name]} */
	"\x4" "send",
	"\x6" "server",        /* {servername} [port] */
	"\x4" "show",
	"\x3" "ttl",           /* {seconds} */
	"\x6" "update",        /* (add|delete) {domain-name} ... */
	"\x4" "zone",           /* {zonename} */
	NULL
};

cmd_handle_f cmd_handle[] = {
	cmd_add,
	cmd_answer,
	cmd_class,
	cmd_debug,
	cmd_del,
	cmd_del,         /* delete/del synonyms */
	cmd_gsstsig,
	cmd_key,
	cmd_local,
	cmd_oldgsstsig,
	cmd_prereq,
	cmd_realm,
	cmd_send,
	cmd_server,
	cmd_show,
	cmd_ttl,
	cmd_update,
	cmd_zone,
};

/* {prereq} command table. */
const char* pq_array[] = {
        "\x8" "nxdomain",
        "\x7" "nxrrset",
        "\x8" "yxdomain",
        "\x7" "yxrrset",
        NULL
};
enum {
	PQ_NXDOMAIN = 0,
	PQ_NXRRSET,
	PQ_YXDOMAIN,
	PQ_YXRRSET,
	UP_ADD,
	UP_DEL
};

/* RR parser flags */
enum {
	PARSE_NODEFAULT = 1 << 0, /* Do not fill defaults. */
	PARSE_NAMEONLY  = 1 << 1  /* Parse only name. */
};

static int dname_isvalid(const char *lp, size_t len) {
	knot_dname_t *dn = knot_dname_new_from_str(lp, len, NULL);
	if (dn == NULL) {
		return 0;
	}
	knot_dname_free(&dn);
	return 1;
}

/* This is probably redundant, but should be a bit faster so let's keep it. */
static int parse_full_rr(scanner_t *s, const char* lp)
{
	if (scanner_process(lp, lp + strlen(lp), 0, s) < 0) {
		return KNOT_EPARSEFAIL;
	}
	char nl = '\n'; /* Ensure newline after complete RR */
	if (scanner_process(&nl, &nl+sizeof(char), 1, s) < 0) { /* Terminate */
		return KNOT_EPARSEFAIL;
	}
	
	/* Class must not differ from specified. */
	if (s->r_class != s->default_class) {
		char cls_s[16] = {0};
		knot_rrclass_to_string(s->default_class, cls_s, sizeof(cls_s));
		ERR("class mismatch: '%s'\n", cls_s);
		return KNOT_EPARSEFAIL;
	}
	
	return KNOT_EOK;
}

static int parse_partial_rr(scanner_t *s, const char *lp, unsigned flags) {
	int ret = KNOT_EOK;
	char b1[32], b2[32]; /* Should suffice for both class/type */
	
	/* Extract owner. */
	size_t len = strcspn(lp, SEP_CHARS);
	knot_dname_t *owner = knot_dname_new_from_str(lp, len, NULL);
	if (owner == NULL) {
		return KNOT_EPARSEFAIL;
	}
	
	/* ISC nsupdate doesn't do this, but it seems right to me. */
	if (!knot_dname_is_fqdn(owner)) {
		knot_dname_t* suf = knot_dname_new_from_wire(s->zone_origin,
		                                             s->zone_origin_length,
		                                             NULL);
		knot_dname_cat(owner, suf);
		knot_dname_free(&suf);
	}
	
	s->r_owner_length = knot_dname_size(owner);
	memcpy(s->r_owner, knot_dname_name(owner), s->r_owner_length);
	lp = tok_skipspace(lp + len);
	
	/* Initialize */
	s->r_type = KNOT_RRTYPE_ANY;
	s->r_class = s->default_class;
	s->r_data_length = 0;
	if (flags & PARSE_NODEFAULT) {
		s->r_ttl = 0;
	} else {
		s->r_ttl = s->default_ttl;
	}
	
	/* Parse only name? */
	if (flags & PARSE_NAMEONLY) {
		knot_dname_free(&owner);
		return KNOT_EOK;
	}

	/* Now there could be [ttl] [class] [type [data...]]. */
	/*! \todo support for fancy time format in ttl */
	char *np = NULL;
	long ttl = strtol(lp, &np, 10);
	if (ttl >= 0 && np && (*np == '\0' || isspace(*np))) {
		s->r_ttl = ttl;
		DBG("%s: parsed ttl=%lu\n", __func__, ttl);
		lp = tok_skipspace(np);
	}
	
	len = strcspn(lp, SEP_CHARS); /* Try to find class */
	memset(b1, 0, sizeof(b1));
	strncpy(b1, lp, len < sizeof(b1) ? len : sizeof(b1));

	uint16_t v;
	if (knot_rrclass_from_string(b1, &v) == 0) {
		s->r_class = v;
		DBG("%s: parsed class=%u\n", __func__, s->r_class);
		lp = tok_skipspace(lp + len);
	}
	
	/* Class must not differ from specified. */
	if (s->r_class != s->default_class) {
		char cls_s[16] = {0};
		knot_rrclass_to_string(s->default_class, cls_s, sizeof(cls_s));
		ERR("class mismatch: '%s'\n", cls_s);
		knot_dname_free(&owner);
		return KNOT_EPARSEFAIL;
	}
	
	len = strcspn(lp, SEP_CHARS); /* Type */
	memset(b2, 0, sizeof(b2));
	strncpy(b2, lp, len < sizeof(b2) ? len : sizeof(b2));
	if (knot_rrtype_from_string(b2, &v) == 0) {
		s->r_type = v;
		DBG("%s: parsed type=%u '%s'\n", __func__, s->r_type, b2);
		lp = tok_skipspace(lp + len);
	}
	
	/* Remainder */
	if (*lp == '\0') { 
		knot_dname_free(&owner);
		return ret; /* No RDATA */
	}
	
	/* Synthetize full RR line to prevent consistency errors. */
	char *owner_s = knot_dname_to_str(owner);
	knot_rrclass_to_string(s->r_class, b1, sizeof(b1));
	knot_rrtype_to_string(s->r_type,   b2, sizeof(b2));
	
	/* Need to parse rdata, synthetize input. */
	char *rr = sprintf_alloc("%s %u %s %s %s\n",
	                         owner_s, s->r_ttl, b1, b2, lp);
	if (scanner_process(rr, rr + strlen(rr), 1, s) < 0) {
		ret = KNOT_EPARSEFAIL;
	}
	
	free(owner_s);
	free(rr);
	knot_dname_free(&owner);
	return ret;
}

static server_t *parse_host(const char *lp, const char* default_port)
{
	/* Extract server address. */
	server_t *srv = NULL;
	size_t len = strcspn(lp, SEP_CHARS);
	char *addr = strndup(lp, len);
	if (!addr) return NULL;
	DBG("%s: parsed addr: %s\n", __func__, addr);
	
	/* Store port/service if present. */
	lp = tok_skipspace(lp + len);
	if (*lp == '\0') {
		srv = server_create(addr, default_port);
		free(addr);
		return srv;
	}

	len = strcspn(lp, SEP_CHARS);
	char *port = strndup(lp, len);
	if (!port) {
		free(addr);
		return NULL;
	}
	DBG("%s: parsed port: %s\n", __func__, port);
	
	/* Create server struct. */
	srv = server_create(addr, port);
	free(addr);
	free(port);
	return srv;
}

static int pkt_append(params_t *p, int sect)
{
	/* Check packet state first. */
	int ret = KNOT_EOK;
	nsupdate_params_t *npar = NSUP_PARAM(p);
	scanner_t *s = npar->rrp;
	if (!npar->pkt) {
		npar->pkt = create_empty_packet(KNOT_PACKET_PREALLOC_RESPONSE,
		                                MAX_PACKET_SIZE);
		knot_question_t q;
		q.qclass = p->class_num;
		q.qtype = p->type_num;
		q.qname = knot_dname_new_from_wire(s->zone_origin,
		                                   s->zone_origin_length,
		                                   NULL);
		ret = knot_query_set_question(npar->pkt, &q);
		knot_dname_release(q.qname); /* Already on wire. */
		if (ret != KNOT_EOK) {
			return ret;
		}
		knot_query_set_opcode(npar->pkt, KNOT_OPCODE_UPDATE);
		
		/* Reserve space for TSIG. */
		if (p->key.name) {
			knot_packet_set_tsig_size(npar->pkt,
			                          tsig_wire_maxsize(&p->key));
		}
	}
	
	/* Create RDATA (not for NXRRSET prereq). */
	knot_rdata_t *rd = knot_rdata_new();
	const knot_rrtype_descriptor_t *rdesc = NULL;
	rdesc = knot_rrtype_descriptor_by_type(s->r_type);
	if (s->r_data_length > 0 && sect != PQ_NXRRSET) {
		size_t pos = 0;
		ret = knot_rdata_from_wire(rd, s->r_data, &pos,
		                           s->r_data_length, s->r_data_length,
		                           rdesc);
		if (ret != KNOT_EOK) {
			DBG("%s: failed to created rd from wire - %s\n",
			    __func__, knot_strerror(ret));
			knot_rdata_free(&rd);
			return ret;
		}
	}
	
	/* Form a rrset. */
	knot_dname_t *o = knot_dname_new_from_wire(s->r_owner, s->r_owner_length, NULL);
	knot_rrset_t *rr = knot_rrset_new(o, s->r_type, s->r_class, s->r_ttl);
	if (!rr) {
		DBG("%s: failed to create rrset - %s\n",
		    __func__, knot_strerror(ret));
		knot_rdata_free(&rd);
		return KNOT_ENOMEM;
	}
	knot_dname_release(o);
	
	/* Append rdata. */
	ret = knot_rrset_add_rdata(rr, rd);
	if (ret != KNOT_EOK) {
		DBG("%s: failed to add rdata - %s\n",
		    __func__, knot_strerror(ret));
		knot_rdata_free(&rd);
		knot_rrset_free(&rr);
		return ret;
	}

	/* Add to correct section.
	 * ZONES  ... QD section.
	 * UPDATE ... NS section. 
	 * PREREQ ... AN section.
	 * ADDIT. ... same.
	 */
	switch(sect) {
	case UP_ADD:
	case UP_DEL:
		ret = knot_response_add_rrset_authority(npar->pkt, rr, 0, 0, 0, 0);
		break;
	case PQ_NXDOMAIN:
	case PQ_NXRRSET:
	case PQ_YXDOMAIN:
	case PQ_YXRRSET:
		ret = knot_response_add_rrset_answer(npar->pkt, rr, 0, 0, 0, 0);
		break;
	default:
		assert(0); /* Should never happen. */
		break;
	}
	
	if (ret != KNOT_EOK) {
		DBG("%s: failed to append rdata to appropriate section - %s\n",
		    __func__, knot_strerror(ret));
		if (ret == KNOT_ESPACE) {
			ERR("exceeded UPDATE message maximum size %zu\n",
			    knot_packet_max_size(npar->pkt));
		}
	}
	
	return ret;
}

static int pkt_sendrecv(params_t *params, server_t *srv,
                        uint8_t *qwire, size_t qlen,
                        uint8_t *rwire, size_t rlen)
{
	/*! \todo Bind to local if specified by params. */
	
	int sock = send_msg(params, KNOT_RRTYPE_SOA, srv, qwire, qlen);
	DBG("%s: send_msg = %d\n", __func__, sock);
	if (sock < 0) return sock;
	
	/* Wait for reception. */
	int rb = receive_msg(params, KNOT_RRTYPE_SOA, sock, rwire, rlen);
	DBG("%s: receive_msg = %d\n", __func__, rb);
	close(sock);
	
	return rb;
}

static int nsupdate_process_line(char *lp, int len, void *arg)
{
	params_t *params = (params_t *)arg;
	
	if (lp[len - 1] == '\n') lp[len - 1] = '\0'; /* Discard nline */
	int ret = tok_find(lp, cmd_array);
	if (ret < 0) return ret; /* Syntax error */
	
	const char *cmd = cmd_array[ret];
	const char *val = tok_skipspace(lp + TOK_L(cmd));
	ret = cmd_handle[ret](val, params);
	if (ret != KNOT_EOK) {
		ERR("operation '%s' failed\n", TOK_S(cmd));
		DBG("reason - %s\n", knot_strerror(ret));
	}
	
	return ret;
}

static int nsupdate_process(params_t *params, FILE *fp)
{
	/* Process lines. */
	int ret = tok_process_lines(fp, nsupdate_process_line, params);
	
	/* Check for longing query. */
	nsupdate_params_t *npar = NSUP_PARAM(params);
	if (npar->pkt && ret == KNOT_EOK) {
		cmd_send("", params);
	}
	
	/* Free last answer. */
	knot_packet_free(&npar->resp);

	return ret;
}

int nsupdate_exec(params_t *params)
{
	if (!params) {
		return KNOT_EINVAL;
	}
	
	int ret = KNOT_EOK;
	nsupdate_params_t* npar = NSUP_PARAM(params);

	/* If not file specified, use stdin. */
	if (EMPTY_LIST(npar->qfiles)) {
		return nsupdate_process(params, stdin);
	}

	/* Read from each specified file. */
	strnode_t *n = NULL;
	WALK_LIST(n, npar->qfiles) {
		if (strcmp(n->str, "-") == 0) {
			ret = nsupdate_process(params, stdin);
			continue;
		}
		FILE *fp = fopen(n->str, "r");
		if (!fp) {
			ERR("could not open '%s': %s\n",
			    n->str, strerror(errno));
			return KNOT_ERROR;
		}
		ret = nsupdate_process(params, fp);
		fclose(fp);
	}

	return ret;
}

int cmd_update(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	/* update is optional token, next add|del|delete */
	int bp = tok_find(lp, cmd_array);
	if (bp < 0) return bp; /* Syntax error. */
	
	/* allow only specific tokens */
	cmd_handle_f *h = cmd_handle;
	if (h[bp] != cmd_add && h[bp] != cmd_del) {
		ERR("unexpected token '%s' after 'update', allowed: '%s'\n",
		    lp, "{add|del|delete}");
		return KNOT_EPARSEFAIL;
	}
	
	return h[bp](tok_skipspace(lp + TOK_L(cmd_array[bp])), params);
}


int cmd_add(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	scanner_t *rrp = NSUP_PARAM(params)->rrp;
	if (parse_full_rr(rrp, lp) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}
	
	/* Parsed RR */
	DBG("%s: parsed rr cls=%u, ttl=%u, type=%u (rdata len=%u)\n",
	    __func__, rrp->r_class, rrp->r_ttl,rrp->r_type, rrp->r_data_length);

	return pkt_append(params, UP_ADD); /* Append to packet. */
}

int cmd_del(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	scanner_t *rrp = NSUP_PARAM(params)->rrp;
	if (parse_partial_rr(rrp, lp, PARSE_NODEFAULT) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}
	
	/* Check owner name. */
	if (rrp->r_owner_length == 0) {
		ERR("failed to parse prereq owner name '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}
	
	rrp->r_ttl = 0; /* Set TTL = 0 when deleting. */
	
	/* When deleting whole RRSet, use ANY class */
	if (rrp->r_data_length == 0) {
		rrp->r_class = KNOT_CLASS_ANY;
	} else {
		rrp->r_class = KNOT_CLASS_NONE;
	}
	
	/* Parsed RR */
	DBG("%s: parsed rr cls=%u, ttl=%u, type=%u (rdata len=%u)\n",
	    __func__, rrp->r_class, rrp->r_ttl,rrp->r_type, rrp->r_data_length);
	
	return pkt_append(params, UP_DEL); /* Append to packet. */
}

int cmd_class(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	uint16_t cls;

	if (knot_rrclass_from_string(lp, &cls) != 0) {
		ERR("failed to parse class '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	} else {
		params->class_num = cls;
		scanner_t *s = NSUP_PARAM(params)->rrp;
		s->default_class = params->class_num;
	}
	
	return KNOT_EOK;
}

int cmd_ttl(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	uint32_t ttl = 0;
	params_parse_num(lp, &ttl);
	nsupdate_params_set_ttl(params, ttl);
	
	return KNOT_EOK;
}

int cmd_debug(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	params_flag_verbose(params);
	msg_enable_debug(1);
	return KNOT_EOK;
}

int cmd_prereq_domain(const char *lp, params_t *params, unsigned type)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	scanner_t *s = NSUP_PARAM(params)->rrp;
	int ret = parse_partial_rr(s, lp, PARSE_NODEFAULT|PARSE_NAMEONLY);
	if (ret != KNOT_EOK) {
		return ret;
	}

	return ret;
}

int cmd_prereq_rrset(const char *lp, params_t *params, unsigned type)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	scanner_t *rrp = NSUP_PARAM(params)->rrp;
	if (parse_partial_rr(rrp, lp, 0) != KNOT_EOK) {
		return KNOT_EPARSEFAIL;
	}
	
	/* Check owner name. */
	if (rrp->r_owner_length == 0) {
		ERR("failed to parse prereq owner name '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}

	/* Parsed RR */
	DBG("%s: parsed rr cls=%u, ttl=%u, type=%u (rdata len=%u)\n",
	    __func__, rrp->r_class, rrp->r_ttl,rrp->r_type, rrp->r_data_length);
	
	return KNOT_EOK;
}

int cmd_prereq(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	/* Scan prereq specifier ([ny]xrrset|[ny]xdomain) */
	int ret = KNOT_EOK;
	int bp = tok_find(lp, pq_array);
	if (bp < 0) return bp; /* Syntax error. */
	
	const char *tok = pq_array[bp];
	DBG("%s: type %s\n", __func__, TOK_S(tok));
	lp = tok_skipspace(lp + TOK_L(tok));
	switch(bp) {
	case PQ_NXDOMAIN:
	case PQ_YXDOMAIN:
		ret = cmd_prereq_domain(lp, params, bp);
		break;
	case PQ_NXRRSET:
	case PQ_YXRRSET:
		ret = cmd_prereq_rrset(lp, params, bp);
		break;
	default:
		return KNOT_ERROR;
	}
	
	/* Append to packet. */
	if (ret == KNOT_EOK) {
		scanner_t *s = NSUP_PARAM(params)->rrp;
		s->r_ttl = 0; /* Set TTL = 0 for prereq. */
		/* YX{RRSET,DOMAIN} - cls ANY */
		if (bp == PQ_YXRRSET || bp == PQ_YXDOMAIN) {
			s->r_class = KNOT_CLASS_ANY;
		} else { /* NX{RRSET,DOMAIN} - cls NONE */
			s->r_class = KNOT_CLASS_NONE;
		}
		
		ret = pkt_append(params, bp);
	}
	
	return ret;
}

int cmd_send(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	DBG("sending packet\n");
	
	/* Create wireformat. */
	int ret = KNOT_EOK;
	uint8_t *wire = NULL;
	size_t len = 0;
	nsupdate_params_t *npar = NSUP_PARAM(params);
	if ((ret = knot_packet_to_wire(npar->pkt, &wire, &len))!= KNOT_EOK) {
		ERR("couldn't serialize packet, %s\n", knot_strerror(ret));
		return ret;
	}
	
	/* Sign if possible. */
	size_t dlen = 0;
	uint8_t *digest = NULL;
	size_t maxlen = knot_packet_max_size(npar->pkt);
	if (params->key.name) {
		dlen = tsig_alg_digest_length(params->key.algorithm);
		digest = malloc(dlen);
		ret = knot_tsig_sign(wire, &len, maxlen, NULL, 0,
		                     digest, &dlen, &params->key, 0, 0);
		if (ret != KNOT_EOK) {
			ERR("failed to sign UPDATE message - %s\n",
			    knot_strerror(ret));
			free(digest);
			return ret;
		}
	}
	
	if (EMPTY_LIST(params->servers)) return KNOT_EINVAL;
	server_t *srv = TAIL(params->servers);
	
	/* Send/recv message (N retries). */
	int retries = params->retries;
	if (params->protocol == PROTO_TCP) {
		retries = 1; /* No retries for TCP. */
	}
	int rb = 0;
	for (; retries > 0; --retries) {
		memset(npar->rwire, 0, MAX_PACKET_SIZE);
		rb = pkt_sendrecv(params, srv, wire, len,
		                  npar->rwire, MAX_PACKET_SIZE);
		if (rb > 0) break;
	}
	
	/* Clear previous response. */
	if (npar->resp) knot_packet_free(&npar->resp);
	if (rb <= 0) {
		free(digest);
		return KNOT_ECONNREFUSED;
	}
	
	/* Clear sent packet and parse response. */
	knot_packet_free_rrsets(npar->pkt);
	knot_packet_free(&npar->pkt);
	npar->resp = knot_packet_new(KNOT_PACKET_PREALLOC_RESPONSE);
	if (!npar->resp) {
		free(digest);
		return KNOT_ENOMEM;
	}
	ret = knot_packet_parse_from_wire(npar->resp, npar->rwire, rb, 0, 0);
	if (ret != KNOT_EOK) {
		ERR("failed to parse response, %s\n", knot_strerror(ret));
		free(digest);
		return ret;
	}
	
	/* Check TSIG if required. */
	const char *ep = "; TSIG error with server";
	const knot_rrset_t *tsig_rr = knot_packet_tsig(npar->resp);
	if (digest && !tsig_rr) {
		ret = KNOT_ENOTSIG;
	} else if (digest) {
		ret = knot_tsig_client_check(tsig_rr, npar->rwire, rb,
		                             digest, dlen, &params->key,
		                             0);
	}
	free(digest); /* Not needed anymore. */
	if (ret != KNOT_EOK) { /* Collect TSIG error. */
		fprintf(stderr, "%s: %s\n", ep, knot_strerror(ret));
		return ret;
	}
	
	/* Check return code. */
	knot_lookup_table_t *rcode;
	int rc = knot_packet_rcode(npar->resp);
	DBG("%s: received rcode=%d\n", __func__, rc);
	rcode = knot_lookup_by_id(rcodes, rc);
	ERR("update failed: %s\n", rcode->name);
	
	/*! \todo Should we check TC bit? */
	
	return KNOT_EOK;
}

int cmd_zone(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	/* Check zone name. */
	size_t len = strcspn(lp, SEP_CHARS);
	if (!dname_isvalid(lp, len)) {
		ERR("failed to parse zone '%s'\n", lp);
		return KNOT_EPARSEFAIL;
	}
	
	/* Extract name. */
	char *zone = strndup(lp, len);
	nsupdate_params_set_origin(params, zone);
	free(zone);
	return KNOT_EOK;
}

int cmd_server(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	/* Parse host. */
	server_t *srv = parse_host(lp, DEFAULT_DNS_PORT);
	
	/* Enqueue. */
	if (!srv) return KNOT_ENOMEM;
	
	add_tail(&params->servers, (node *)srv);
	return KNOT_EOK;
}

int cmd_local(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	
	/* Parse host. */
	nsupdate_params_t *npar = NSUP_PARAM(params);
	if ((npar->srcif = parse_host(lp, "0")) == NULL) {
		return KNOT_ENOMEM;
	}
	
	return KNOT_EOK;
}

int cmd_show(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	nsupdate_params_t *npar = NSUP_PARAM(params);
	/* Show current packet. */
	if (!npar->pkt) return KNOT_EOK;
	printf("Outgoing update query:\n");
	size_t len = knot_packet_size(npar->pkt);
	print_packet(params->format, npar->pkt, len, 0, 0.0f, 1);
	return KNOT_EOK;
}

int cmd_answer(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	nsupdate_params_t *npar = NSUP_PARAM(params);
	/* Show current answer. */
	if (!npar->resp) return KNOT_EOK;
	printf("Answer:\n");
	size_t len = knot_packet_size(npar->resp);
	print_packet(params->format, npar->resp, len, 0, 0.0f, 1);
	return KNOT_EOK;
}

int cmd_key(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	char *kstr = strdup(lp); /* Convert to default format. */
	if (!kstr) return KNOT_ENOMEM;
	
	
	int ret = KNOT_EOK;
	size_t len = strcspn(lp, SEP_CHARS);
	if(kstr[len] == '\0') {
		ERR("command 'key' without {secret} specified\n");
		ret = KNOT_EINVAL;
	} else {
		kstr[len] = ':'; /* Replace ' ' with ':' sep */
		ret = params_parse_tsig(kstr, &params->key);
	}
	
	free(kstr);
	return ret;
}

/*
 *   Not implemented.
 */

int cmd_gsstsig(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	return KNOT_ENOTSUP;
}

int cmd_oldgsstsig(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	return KNOT_ENOTSUP;
}

int cmd_realm(const char* lp, params_t *params)
{
	DBG("%s: lp='%s'\n", __func__, lp);
	return KNOT_ENOTSUP;
}
