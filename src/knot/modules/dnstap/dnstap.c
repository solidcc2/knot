/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include <netinet/in.h>
#include <sys/socket.h>

#include "contrib/dnstap/dnstap.h"
#include "contrib/dnstap/dnstap.pb-c.h"
#include "contrib/dnstap/message.h"
#include "contrib/dnstap/writer.h"
#include "contrib/time.h"
#include "knot/include/module.h"

#define MOD_SINK		"\x04""sink"
#define MOD_IDENTITY		"\x08""identity"
#define MOD_VERSION		"\x07""version"
#define MOD_QUERIES		"\x0B""log-queries"
#define MOD_RESPONSES		"\x0D""log-responses"
#define MOD_WITH_QUERIES	"\x16""responses-with-queries"

const yp_item_t dnstap_conf[] = {
	{ MOD_SINK,         YP_TSTR,  YP_VNONE },
	{ MOD_IDENTITY,     YP_TSTR,  YP_VNONE },
	{ MOD_VERSION,      YP_TSTR,  YP_VNONE },
	{ MOD_QUERIES,      YP_TBOOL, YP_VBOOL = { true } },
	{ MOD_RESPONSES,    YP_TBOOL, YP_VBOOL = { true } },
	{ MOD_WITH_QUERIES, YP_TBOOL, YP_VBOOL = { false } },
	{ NULL }
};

int dnstap_conf_check(knotd_conf_check_args_t *args)
{
	knotd_conf_t sink = knotd_conf_check_item(args, MOD_SINK);
	if (sink.count == 0 || sink.single.string[0] == '\0') {
		args->err_str = "no sink specified";
		return KNOT_EINVAL;
	}

	return KNOT_EOK;
}

typedef struct {
	struct fstrm_iothr *iothread;
	char *identity;
	size_t identity_len;
	char *version;
	size_t version_len;
	bool with_queries;
} dnstap_ctx_t;

static knotd_state_t log_message(knotd_state_t state, const knot_pkt_t *pkt,
                                 knotd_qdata_t *qdata, knotd_mod_t *mod)
{
	assert(pkt && qdata && mod);

	/* Skip empty packet. */
	if (state == KNOTD_STATE_NOOP) {
		return state;
	}

	dnstap_ctx_t *ctx = knotd_mod_ctx(mod);

	struct fstrm_iothr_queue *ioq =
		fstrm_iothr_get_input_queue_idx(ctx->iothread, qdata->params->thread_id);

	/* Unless we want to measure the time it takes to process each query,
	 * we can treat Q/R times the same. */
	struct timespec tv = { 0 };
	clock_gettime(CLOCK_REALTIME, &tv);

	/* Determine query / response. */
	Dnstap__Message__Type msgtype = DNSTAP__MESSAGE__TYPE__AUTH_QUERY;
	if (knot_wire_get_opcode(pkt->wire) == KNOT_OPCODE_UPDATE) {
		msgtype = DNSTAP__MESSAGE__TYPE__UPDATE_QUERY;
	}
	if (knot_wire_get_qr(pkt->wire)) {
		msgtype++; // NOTE relies on RESPONSE always being an enum+1 of QUERY
	}

	/* Create a dnstap message. */
	Dnstap__Message msg;
	int ret = dt_message_fill(&msg, msgtype,
	                          (const struct sockaddr *)knotd_qdata_remote_addr(qdata),
	                          (const struct sockaddr *)knotd_qdata_local_addr(qdata),
	                          qdata->params->proto, pkt->wire, pkt->size, &tv);
	if (ret != KNOT_EOK) {
		return state;
	}

	Dnstap__Dnstap dnstap = DNSTAP__DNSTAP__INIT;
	dnstap.type = DNSTAP__DNSTAP__TYPE__MESSAGE;
	dnstap.message = &msg;

	/* Set message version and identity. */
	if (ctx->identity_len > 0) {
		dnstap.identity.data = (uint8_t *)ctx->identity;
		dnstap.identity.len = ctx->identity_len;
		dnstap.has_identity = 1;
	}
	if (ctx->version_len > 0) {
		dnstap.version.data = (uint8_t *)ctx->version;
		dnstap.version.len = ctx->version_len;
		dnstap.has_version = 1;
	}

	/* Also add query message if 'responses-with-queries' is enabled and this is a response. */
	if (ctx->with_queries &&
	    msgtype == DNSTAP__MESSAGE__TYPE__AUTH_RESPONSE &&
	    qdata->query != NULL)
	{
		msg.query_message.len = qdata->query->size;
		msg.query_message.data = qdata->query->wire;
		msg.has_query_message = 1;
	}

	/* Pack the message. */
	uint8_t *frame = NULL;
	size_t size = 0;
	dt_pack(&dnstap, &frame, &size);
	if (frame == NULL) {
		return state;
	}

	/* Submit a request. */
	fstrm_res res = fstrm_iothr_submit(ctx->iothread, ioq, frame, size,
	                                   fstrm_free_wrapper, NULL);
	if (res != fstrm_res_success) {
		free(frame);
		return state;
	}

	return state;
}

/*! \brief Submit message - query. */
static knotd_state_t dnstap_message_log_query(knotd_state_t state, knot_pkt_t *pkt,
                                              knotd_qdata_t *qdata, knotd_mod_t *mod)
{
	assert(qdata);

	return log_message(state, qdata->query, qdata, mod);
}

/*! \brief Submit message - response. */
static knotd_state_t dnstap_message_log_response(knotd_state_t state, knot_pkt_t *pkt,
                                                 knotd_qdata_t *qdata, knotd_mod_t *mod)
{
	return log_message(state, pkt, qdata, mod);
}

/*! \brief Create a UNIX socket sink. */
static struct fstrm_writer* dnstap_unix_writer(const char *path)
{
	struct fstrm_unix_writer_options *opt = NULL;
	struct fstrm_writer_options *wopt = NULL;
	struct fstrm_writer *writer = NULL;

	opt = fstrm_unix_writer_options_init();
	if (opt == NULL) {
		goto finish;
	}
	fstrm_unix_writer_options_set_socket_path(opt, path);

	wopt = fstrm_writer_options_init();
	if (wopt == NULL) {
		goto finish;
	}
	fstrm_writer_options_add_content_type(wopt, DNSTAP_CONTENT_TYPE,
	                                      strlen(DNSTAP_CONTENT_TYPE));
	writer = fstrm_unix_writer_init(opt, wopt);

finish:
	fstrm_unix_writer_options_destroy(&opt);
	fstrm_writer_options_destroy(&wopt);
	return writer;
}

static struct fstrm_writer* dnstap_tcp_writer(const char *address, const char *port)
{
	struct fstrm_tcp_writer_options *opt = NULL;
	struct fstrm_writer_options *wopt = NULL;
	struct fstrm_writer *writer = NULL;

	opt =  fstrm_tcp_writer_options_init();
	if (opt == NULL) {
		goto finish;
	}

	fstrm_tcp_writer_options_set_socket_address(opt, address);
	fstrm_tcp_writer_options_set_socket_port(opt, port);

	wopt = fstrm_writer_options_init();
	if (wopt == NULL) {
		goto finish;
	}
	fstrm_writer_options_add_content_type(wopt, DNSTAP_CONTENT_TYPE,
	                                      strlen(DNSTAP_CONTENT_TYPE));
	writer = fstrm_tcp_writer_init(opt, wopt);
finish:
	fstrm_tcp_writer_options_destroy(&opt);
	fstrm_writer_options_destroy(&wopt);
	return writer;
}

/*! \brief Create a basic file writer sink. */
static struct fstrm_writer* dnstap_file_writer(const char *path)
{
	struct fstrm_file_options *fopt = NULL;
	struct fstrm_writer_options *wopt = NULL;
	struct fstrm_writer *writer = NULL;

	fopt = fstrm_file_options_init();
	if (fopt == NULL) {
		goto finish;
	}
	fstrm_file_options_set_file_path(fopt, path);

	wopt = fstrm_writer_options_init();
	if (wopt == NULL) {
		goto finish;
	}
	fstrm_writer_options_add_content_type(wopt, DNSTAP_CONTENT_TYPE,
	                                      strlen(DNSTAP_CONTENT_TYPE));
	writer = fstrm_file_writer_init(fopt, wopt);

finish:
	fstrm_file_options_destroy(&fopt);
	fstrm_writer_options_destroy(&wopt);
	return writer;
}

/*! \brief Create a log sink according to the path string. */
static struct fstrm_writer* dnstap_writer(knotd_mod_t *mod, const char *path)
{
	const char *unix_prefix = "unix:";
	const size_t unix_prefix_len = strlen(unix_prefix);

	const char *tcp_prefix = "tcp:";
	const size_t tcp_prefix_len = strlen(tcp_prefix);

	const size_t path_len = strlen(path);

	/* UNIX socket prefix. */
	if (path_len > unix_prefix_len &&
	    strncmp(path, unix_prefix, unix_prefix_len) == 0) {
		knotd_mod_log(mod, LOG_DEBUG, "using sink UNIX socket '%s'", path);
		return dnstap_unix_writer(path + unix_prefix_len);
	/* TCP socket prefix. */
	} else if (path_len > tcp_prefix_len &&
	           strncmp(path, tcp_prefix, tcp_prefix_len) == 0) {
		char addr[INET6_ADDRSTRLEN] = { 0 };
		const char *delimiter = strchr(path + tcp_prefix_len, '@');
		if (delimiter == NULL) {
			return NULL;
		}
		size_t addr_len = delimiter - path - tcp_prefix_len;
		if (addr_len >= sizeof(addr)) {
			return NULL;
		}
		memcpy(addr, path + tcp_prefix_len, addr_len);
		knotd_mod_log(mod, LOG_DEBUG, "using sink TCP address '%s' port '%s'",
		              addr, delimiter + 1);
		return dnstap_tcp_writer(addr, delimiter + 1);
	/* File path. */
	} else {
		knotd_mod_log(mod, LOG_DEBUG, "using sink file '%s'", path);
		return dnstap_file_writer(path);
	}
}

int dnstap_load(knotd_mod_t *mod)
{
	/* Create dnstap context. */
	dnstap_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return KNOT_ENOMEM;
	}

	/* Set identity. */
	knotd_conf_t conf = knotd_conf_mod(mod, MOD_IDENTITY);
	if (conf.count == 1) {
		ctx->identity = (conf.single.string != NULL) ?
		                strdup(conf.single.string) : NULL;
	} else {
		knotd_conf_t host = knotd_conf_env(mod, KNOTD_CONF_ENV_HOSTNAME);
		ctx->identity = strdup(host.single.string);
	}
	ctx->identity_len = (ctx->identity != NULL) ? strlen(ctx->identity) : 0;

	/* Set version. */
	conf = knotd_conf_mod(mod, MOD_VERSION);
	if (conf.count == 1) {
		ctx->version = (conf.single.string != NULL) ?
		               strdup(conf.single.string) : NULL;
	} else {
		knotd_conf_t version = knotd_conf_env(mod, KNOTD_CONF_ENV_VERSION);
		ctx->version = strdup(version.single.string);
	}
	ctx->version_len = (ctx->version != NULL) ? strlen(ctx->version) : 0;

	/* Set responses-with-queries. */
	conf = knotd_conf_mod(mod, MOD_WITH_QUERIES);
	ctx->with_queries = conf.single.boolean;

	/* Set sink. */
	conf = knotd_conf_mod(mod, MOD_SINK);
	const char *sink = conf.single.string;

	/* Set log_queries. */
	conf = knotd_conf_mod(mod, MOD_QUERIES);
	const bool log_queries = conf.single.boolean;

	/* Set log_responses. */
	conf = knotd_conf_mod(mod, MOD_RESPONSES);
	const bool log_responses = conf.single.boolean;

	/* Initialize the writer and the options. */
	struct fstrm_writer *writer = dnstap_writer(mod, sink);
	if (writer == NULL) {
		goto fail;
	}

	struct fstrm_iothr_options *opt = fstrm_iothr_options_init();
	if (opt == NULL) {
		fstrm_writer_destroy(&writer);
		goto fail;
	}

	/* Initialize queues. */
	fstrm_iothr_options_set_num_input_queues(opt, knotd_mod_threads(mod));

	/* Create the I/O thread. */
	ctx->iothread = fstrm_iothr_init(opt, &writer);
	fstrm_iothr_options_destroy(&opt);
	if (ctx->iothread == NULL) {
		fstrm_writer_destroy(&writer);
		goto fail;
	}

	knotd_mod_ctx_set(mod, ctx);

	/* Hook to the query plan. */
	if (log_queries) {
		knotd_mod_hook(mod, KNOTD_STAGE_BEGIN, dnstap_message_log_query);
	}
	if (log_responses) {
		knotd_mod_hook(mod, KNOTD_STAGE_END, dnstap_message_log_response);
	}

	return KNOT_EOK;
fail:
	knotd_mod_log(mod, LOG_ERR, "failed to initialize sink '%s'", sink);

	free(ctx->identity);
	free(ctx->version);
	free(ctx);

	return KNOT_EINVAL;
}

void dnstap_unload(knotd_mod_t *mod)
{
	dnstap_ctx_t *ctx = knotd_mod_ctx(mod);

	fstrm_iothr_destroy(&ctx->iothread);
	free(ctx->identity);
	free(ctx->version);
	free(ctx);
}

KNOTD_MOD_API(dnstap, KNOTD_MOD_FLAG_SCOPE_ANY,
              dnstap_load, dnstap_unload, dnstap_conf, dnstap_conf_check);
