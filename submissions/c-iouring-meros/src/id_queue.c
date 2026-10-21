#include "id_queue.h"

#include <string.h>

#include "memory.h"

enum { INITIAL_CAPACITY = 256 };

void id_queue_push(id_queue *queue, int id)
{
	if (queue->tail == queue->capacity) {
		size_t live = queue->tail - queue->head;

		if (queue->head >= queue->capacity / 2 && queue->capacity != 0) {
			memmove(queue->items, queue->items + queue->head, live * sizeof *queue->items);
		} else {
			queue->capacity = queue->capacity == 0 ? INITIAL_CAPACITY : queue->capacity * 2;
			queue->items = xrealloc(queue->items, queue->capacity * sizeof *queue->items);
			memmove(queue->items, queue->items + queue->head, live * sizeof *queue->items);
		}
		queue->head = 0;
		queue->tail = live;
	}
	queue->items[queue->tail++] = id;
}

bool id_queue_pop(id_queue *queue, int *id)
{
	if (queue->head == queue->tail) {
		queue->head = queue->tail = 0;
		return false;
	}
	*id = queue->items[queue->head++];
	return true;
}
