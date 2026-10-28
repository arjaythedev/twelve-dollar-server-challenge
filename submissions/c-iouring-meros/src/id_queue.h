/* id_queue: a growable FIFO of connection ids. */
#ifndef ID_QUEUE_H
#define ID_QUEUE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct id_queue {
	int *items;
	size_t head;
	size_t tail;
	size_t capacity;
} id_queue;

void id_queue_push(id_queue *queue, int id);
bool id_queue_pop(id_queue *queue, int *id);

#endif
