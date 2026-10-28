#include "conn.h"

#include "memory.h"

enum { MIN_CARRY_CAP = 2048 };

void conn_table_init(conn_table *table, int id_space, int capacity, const conn_backend *backend, void *backend_data)
{
	/* calloc hands out untouched zero pages, so unused slots cost no memory. */
	table->entries = xcalloc((size_t)id_space, sizeof(conn));
	table->id_space = id_space;
	table->capacity = capacity;
	table->backend = backend;
	table->backend_data = backend_data;
	table->highest_id = 0;
}

void conn_register(conn_table *table, int id, time_t now)
{
	conn *c = conn_get(table, id);

	conn_set(c, CONN_OPEN);
	c->generation++;
	table->open_count++;
	c->last_active = (uint32_t)now;
	if (id > table->highest_id)
		table->highest_id = id;
}

void conn_close(conn_table *table, int id)
{
	conn *c = conn_get(table, id);
	uint32_t generation = c->generation;

	if (conn_has(c, CONN_OPEN))
		table->open_count--;
	free(c->carry);
	free(c->unsent);
	memset(c, 0, sizeof *c);
	c->generation = generation;
	table->backend->release(table, id);
}

void conn_carry_set(conn *c, const char *data, size_t len)
{
	if (len > c->carry_cap) {
		size_t cap = len < MIN_CARRY_CAP ? MIN_CARRY_CAP : len;

		c->carry = xrealloc(c->carry, cap);
		c->carry_cap = (uint32_t)cap;
	}
	memmove(c->carry, data, len);
	c->carry_len = (uint32_t)len;
}

void conn_carry_clear(conn *c)
{
	free(c->carry);
	c->carry = NULL;
	c->carry_len = c->carry_cap = 0;
}

void conn_carry_append(conn *c, const char *data, size_t len)
{
	size_t needed = (size_t)c->carry_len + len;

	if (needed > c->carry_cap) {
		c->carry = xrealloc(c->carry, needed);
		c->carry_cap = (uint32_t)needed;
	}
	memcpy(c->carry + c->carry_len, data, len);
	c->carry_len = (uint32_t)needed;
}

void conn_close_idle(conn_table *table, time_t now, unsigned max_idle_seconds)
{
	for (int id = 0; id <= table->highest_id; id++) {
		conn *c = conn_get(table, id);

		if (conn_has(c, CONN_OPEN) && !conn_has(c, CONN_AWAITING_BATCH) &&
		    (time_t)c->last_active + max_idle_seconds < now)
			conn_close(table, id);
	}
}

void conn_drain_start(conn_table *table, int id, uint64_t now_ns)
{
	conn *c = conn_get(table, id);

	if (table->drain_tail == table->drain_cap) {
		size_t live = table->drain_tail - table->drain_head;

		if (table->drain_head >= table->drain_cap / 2 && table->drain_cap != 0) {
			memmove(table->drains, table->drains + table->drain_head, live * sizeof *table->drains);
		} else {
			table->drain_cap = table->drain_cap == 0 ? 1024 : table->drain_cap * 2;
			table->drains = xrealloc(table->drains, table->drain_cap * sizeof *table->drains);
			memmove(table->drains, table->drains + table->drain_head, live * sizeof *table->drains);
		}
		table->drain_head = 0;
		table->drain_tail = live;
	}
	conn_set(c, CONN_DRAINING);
	table->drains[table->drain_tail++] = (drain_entry){ id, c->generation, now_ns + (uint64_t)CONN_DRAIN_TIMEOUT_MS * 1000000ULL };
}

void conn_drain_expire(conn_table *table, uint64_t now_ns)
{
	while (table->drain_head != table->drain_tail && table->drains[table->drain_head].deadline_ns <= now_ns) {
		drain_entry entry = table->drains[table->drain_head++];
		conn *c = conn_get(table, entry.id);

		/* The client may have closed already, and the descriptor number may belong to a newer connection. */
		if (conn_has(c, CONN_OPEN) && conn_has(c, CONN_DRAINING) && c->generation == entry.generation)
			conn_close(table, entry.id);
	}
	if (table->drain_head == table->drain_tail)
		table->drain_head = table->drain_tail = 0;
}

unsigned conn_evict_idle(conn_table *table, time_t now, unsigned min_idle_seconds, unsigned max)
{
	unsigned evicted = 0;

	for (int scanned = 0; scanned <= table->highest_id && evicted < max; scanned++) {
		int id = table->evict_cursor;
		conn *c = conn_get(table, id);

		table->evict_cursor = id >= table->highest_id ? 0 : id + 1;
		if (conn_has(c, CONN_OPEN) && !conn_has(c, CONN_AWAITING_BATCH) && !conn_has(c, CONN_SEND_PENDING) &&
		    (time_t)c->last_active + min_idle_seconds <= now) {
			conn_close(table, id);
			evicted++;
		}
	}
	return evicted;
}
