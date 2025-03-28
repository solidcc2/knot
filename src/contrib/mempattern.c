/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

#include <stdlib.h>

#include "contrib/mempattern.h"
#include "contrib/string.h"
#include "contrib/ucw/mempool.h"

static void mm_nofree(void *p)
{
	/* nop */
}

static void *mm_malloc(void *ctx, size_t n)
{
	(void)ctx;
	return malloc(n);
}

void *mm_alloc(knot_mm_t *mm, size_t size)
{
	if (mm) {
		return mm->alloc(mm->ctx, size);
	} else {
		return malloc(size);
	}
}

void *mm_calloc(knot_mm_t *mm, size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0) {
		return NULL;
	}
	if (mm) {
		size_t total_size = nmemb * size;
		if (total_size / nmemb != size) { // Overflow check
			return NULL;
		}
		void *mem = mm_alloc(mm, total_size);
		if (mem == NULL) {
			return NULL;
		}
		return memzero(mem, total_size);
	} else {
		return calloc(nmemb, size);
	}
}

void *mm_realloc(knot_mm_t *mm, void *what, size_t size, size_t prev_size)
{
	if (mm) {
		void *p = mm->alloc(mm->ctx, size);
		if (p == NULL) {
			return NULL;
		} else {
			if (what) {
				memcpy(p, what,
				       prev_size < size ? prev_size : size);
			}
			mm_free(mm, what);
			return p;
		}
	} else {
		return realloc(what, size);
	}
}

char *mm_strdup(knot_mm_t *mm, const char *s)
{
	if (s == NULL) {
		return NULL;
	}
	if (mm) {
		size_t len = strlen(s) + 1;
		void *mem = mm_alloc(mm, len);
		if (mem == NULL) {
			return NULL;
		}
		return memcpy(mem, s, len);
	} else {
		return strdup(s);
	}
}

void mm_free(knot_mm_t *mm, void *what)
{
	if (mm) {
		if (mm->free) {
			mm->free(what);
		}
	} else {
		free(what);
	}
}

void mm_ctx_init(knot_mm_t *mm)
{
	mm->ctx = NULL;
	mm->alloc = mm_malloc;
	mm->free = free;
}

// UBSAN type punning workaround
static void *mp_alloc_wrap(void *ctx, size_t size)
{
	return mp_alloc(ctx, size);
}

void mm_ctx_mempool(knot_mm_t *mm, size_t chunk_size)
{
	mm->ctx = mp_new(chunk_size);
	mm->alloc = mp_alloc_wrap;
	mm->free = mm_nofree;
}
