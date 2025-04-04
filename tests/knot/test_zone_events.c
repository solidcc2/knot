/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include <tap/basic.h>

#include "knot/common/evsched.h"
#include "knot/worker/pool.h"
#include "knot/events/events.h"
#include "knot/zone/zone.h"

static void test_scheduling(zone_t *zone)
{
	const time_t now = time(NULL);
	const unsigned offset = 1000;

	time_t timestamp = 0;
	zone_event_type_t event = 0;

	timestamp = zone_events_get_next(zone, &event);
	ok(timestamp < 0 && event == ZONE_EVENT_INVALID, "nothing planned");

	// scheduling

	zone_events_schedule_at(zone, ZONE_EVENT_EXPIRE, now + offset);
	zone_events_schedule_at(zone, ZONE_EVENT_FLUSH,  now + (offset / 2));

	for (int i = 0; i < ZONE_EVENT_COUNT; i++) {
		time_t t = zone_events_get_time(zone, i);
		bool scheduled = i == ZONE_EVENT_EXPIRE || i == ZONE_EVENT_FLUSH;
		const char *name = zone_events_get_name(i);

		ok((t > 0) == scheduled && name, "event %s (%s)", name,
		   scheduled ? "scheduled" : "not scheduled");
	}

	// queuing

	timestamp = zone_events_get_next(zone, &event);
	ok(timestamp >= now + (offset / 2) && event == ZONE_EVENT_FLUSH, "flush is next");

	zone_events_schedule_at(zone, ZONE_EVENT_FLUSH, (time_t)0);

	timestamp = zone_events_get_next(zone, &event);
	ok(timestamp >= now + offset && event == ZONE_EVENT_EXPIRE, "expire is next");

	zone_events_schedule_at(zone, ZONE_EVENT_EXPIRE, (time_t)0);

	timestamp = zone_events_get_next(zone, &event);
	ok(timestamp < 0 && event == ZONE_EVENT_INVALID, "nothing planned");

	// zone_events_enqueue

	// zone_events_freeze
	// zone_events_start
}

int main(void)
{
	plan_lazy();

	int r;

	evsched_t sched = { 0 };
	worker_pool_t *pool = NULL;
	zone_t zone = { 0 };

	r = evsched_init(&sched, NULL);
	ok(r == KNOT_EOK, "create scheduler");

	pool = worker_pool_create(1);
	ok(pool != NULL, "create worker pool");

	r = zone_events_init(&zone);
	ok(r == KNOT_EOK, "zone events init");

	r = zone_events_setup(&zone, pool, &sched);
	ok(r == KNOT_EOK, "zone events setup");

	test_scheduling(&zone);

	zone_events_deinit(&zone);
	worker_pool_destroy(pool);
	evsched_deinit(&sched);

	return 0;
}
