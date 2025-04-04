/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include "knot/zone/measure.h"

measure_t knot_measure_init(bool measure_whole, bool measure_diff)
{
	assert(!measure_whole || !measure_diff);
	measure_t m = { 0 };
	if (measure_whole) {
		m.how_size = MEASURE_SIZE_WHOLE;
		m.how_ttl = MEASURE_TTL_WHOLE;
	}
	if (measure_diff) {
		m.how_size = MEASURE_SIZE_DIFF;
		m.how_ttl = MEASURE_TTL_DIFF;
	}
	return m;
}

static uint32_t rrset_max_ttl(const struct rr_data *r)
{
	if (r->type != KNOT_RRTYPE_RRSIG) {
		return r->ttl;
	}

	uint32_t res = 0;
	knot_rdata_t *rd = r->rrs.rdata;
	for (int i = 0; i < r->rrs.count; i++) {
		res = MAX(res, knot_rrsig_original_ttl(rd));
		rd = knot_rdataset_next(rd);
	}
	return res;
}

bool knot_measure_node(zone_node_t *node, measure_t *m)
{
	if (m->how_size == MEASURE_SIZE_NONE && (m->how_ttl == MEASURE_TTL_NONE ||
	      (m->how_ttl == MEASURE_TTL_LIMIT && m->max_ttl >= m->limit_max_ttl))) {
		return false;
	}

	int rrset_count = node->rrset_count;
	for (int i = 0; i < rrset_count; i++) {
		if (m->how_size != MEASURE_SIZE_NONE) {
			knot_rrset_t rrset = node_rrset_at(node, i);
			m->zone_size += knot_rrset_size(&rrset);
		}
		if (m->how_ttl != MEASURE_TTL_NONE) {
			m->max_ttl = MAX(m->max_ttl, rrset_max_ttl(&node->rrs[i]));
		}
	}

	if (m->how_size != MEASURE_SIZE_DIFF && m->how_ttl != MEASURE_TTL_DIFF) {
		return true;
	}

	node = binode_counterpart(node);
	rrset_count = node->rrset_count;
	for (int i = 0; i < rrset_count; i++) {
		if (m->how_size == MEASURE_SIZE_DIFF) {
			knot_rrset_t rrset = node_rrset_at(node, i);
			m->zone_size -= knot_rrset_size(&rrset);
		}
		if (m->how_ttl == MEASURE_TTL_DIFF) {
			m->rem_max_ttl = MAX(m->rem_max_ttl, rrset_max_ttl(&node->rrs[i]));
		}
	}

	return true;
}

static uint32_t re_measure_max_ttl(zone_contents_t *zone, uint32_t limit)
{
	measure_t m = {0 };
	m.how_ttl = MEASURE_TTL_LIMIT;
	m.limit_max_ttl = limit;

	zone_tree_it_t it = { 0 };
	int ret = zone_tree_it_double_begin(zone->nodes, zone->nsec3_nodes, &it);
	if (ret != KNOT_EOK) {
		return limit;
	}

	while (!zone_tree_it_finished(&it) && knot_measure_node(zone_tree_it_val(&it), &m)) {
		zone_tree_it_next(&it);
	}
	zone_tree_it_free(&it);

	return m.max_ttl;
}

void knot_measure_finish_zone(measure_t *m, zone_contents_t *zone)
{
	assert(m->how_size == MEASURE_SIZE_WHOLE || m->how_size == MEASURE_SIZE_NONE);
	assert(m->how_ttl == MEASURE_TTL_WHOLE || m->how_ttl == MEASURE_TTL_NONE);
	if (m->how_size == MEASURE_SIZE_WHOLE) {
		zone->size = m->zone_size;
	}
	if (m->how_ttl == MEASURE_TTL_WHOLE) {
		zone->max_ttl = m->max_ttl;
	}
}

void knot_measure_finish_update(measure_t *m, zone_update_t *update)
{
	switch (m->how_size) {
	case MEASURE_SIZE_NONE:
		break;
	case MEASURE_SIZE_WHOLE:
		update->new_cont->size = m->zone_size;
		break;
	case MEASURE_SIZE_DIFF:
		update->new_cont->size = update->zone->contents->size + m->zone_size;
		break;
	}

	switch (m->how_ttl) {
	case MEASURE_TTL_NONE:
		break;
	case MEASURE_TTL_WHOLE:
	case MEASURE_TTL_LIMIT:
		update->new_cont->max_ttl = m->max_ttl;
		break;
	case MEASURE_TTL_DIFF:
		if (m->max_ttl >= update->zone->contents->max_ttl) {
			update->new_cont->max_ttl = m->max_ttl;
		} else if (update->zone->contents->max_ttl > m->rem_max_ttl) {
			update->new_cont->max_ttl = update->zone->contents->max_ttl;
		} else {
			update->new_cont->max_ttl = re_measure_max_ttl(update->new_cont, update->zone->contents->max_ttl);
		}
		break;
	}
}
