#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>
#include "ipc_types.h"

namespace ipc {

namespace {

void split_heap_node(void *heap_base, HeapNode *node, uint32_t size)
{
	uint32_t node_offset = pointer_to_offset(heap_base, node);
	uint32_t alloc_size = size;

	if (alloc_size % alignof(HeapNode))
		alloc_size += alignof(HeapNode)-alloc_size % alignof(HeapNode);

	HeapNode *next = new (offset_to_pointer<void>(heap_base, node_offset + alloc_size)) HeapNode{};
	next->prev_node_offset = node_offset;
	next->next_node_offset = node->next_node_offset;

	node->next_node_offset = node_offset + alloc_size;
}

} // namespace


void queue_read(Queue *queue, void *buf)
{
	unsigned char *queue_base = offset_to_pointer<unsigned char>(queue, queue->buffer_offset);
	uint32_t capacity = queue->size - queue->buffer_offset;

	if (queue->buffer_usage <= capacity - queue->read_pos) {
		std::memcpy(buf, queue_base + queue->read_pos, queue->buffer_usage);
		queue->read_pos += queue->buffer_usage;
	} else {
		uint32_t read_first = capacity - queue->read_pos;
		std::memcpy(buf, queue_base + queue->read_pos, read_first);

		buf = offset_to_pointer<void>(buf, read_first);
		std::memcpy(buf, queue_base, queue->buffer_usage - read_first);

		queue->read_pos = queue->buffer_usage - read_first;
	}

	queue->buffer_usage = 0;
}

void queue_write(Queue *queue, const void *buf, uint32_t size)
{
	unsigned char *queue_base = offset_to_pointer<unsigned char>(queue, queue->buffer_offset);
	uint32_t capacity = queue->size - queue->buffer_offset;

	assert(size <= capacity - queue->buffer_usage);

	if (size <= capacity - queue->write_pos) {
		std::memcpy(queue_base + queue->write_pos, buf, size);
		queue->write_pos += size;
	} else {
		uint32_t write_first = capacity - queue->write_pos;
		std::memcpy(queue_base + queue->write_pos, buf, write_first);

		buf = offset_to_pointer<void>(buf, write_first);
		std::memcpy(queue_base, buf, size - write_first);

		queue->write_pos = size - write_first;
	}

	queue->buffer_usage += size;
}

HeapNode *heap_alloc(Heap *heap, uint32_t size)
{
	void *heap_base = offset_to_pointer<void>(heap, heap->buffer_offset);
	uint32_t capacity = heap->size - heap->buffer_offset;

	if (size > capacity - sizeof(HeapNode))
		return nullptr;

	size += sizeof(HeapNode);
	if (size > capacity - heap->buffer_usage)
		return nullptr;

	// Try the hint first.
	HeapNode *node = static_cast<HeapNode *>(heap_base);
	if (heap->last_free_offset != NULL_OFFSET)
		node = offset_to_pointer<HeapNode>(heap_base, heap->last_free_offset);

	// Forward scan.
	HeapNode *initial = node;

	while (true) {
		assert(check_fourcc(node->magic, "memz"));

		uint32_t node_offset = pointer_to_offset(heap_base, node);
		uint32_t node_real_next = node->next_node_offset == NULL_OFFSET ? capacity : node->next_node_offset;
		uint32_t node_size = node_real_next - node_offset;

		if (!(node->flags & HEAP_FLAG_ALLOCATED) && size < node_size) {
			if (node_size - size >= 4096U) {
				split_heap_node(heap_base, node, size);
				node_real_next = node->next_node_offset;
			}

			node->flags |= HEAP_FLAG_ALLOCATED;
			heap->buffer_usage += node_real_next - node_offset;
			return node;
		}

		if (node->next_node_offset == NULL_OFFSET)
			break;

		node = offset_to_pointer<HeapNode>(heap_base, node->next_node_offset);
	}

	// Reverse scan.
	if (initial->prev_node_offset == NULL_OFFSET)
		return nullptr;

	node = offset_to_pointer<HeapNode>(heap_base, initial->prev_node_offset);

	while (true) {
		assert(check_fourcc(node->magic, "memz"));

		uint32_t node_offset = pointer_to_offset(heap_base, node);
		uint32_t node_real_next = node->next_node_offset == NULL_OFFSET ? capacity : node->next_node_offset;
		uint32_t node_size = node_real_next - node_offset;

		if (!(node->flags & HEAP_FLAG_ALLOCATED) && size < node_size) {
			if (node_size - size >= 4096U)
				split_heap_node(heap_base, node, size);

			node->flags |= HEAP_FLAG_ALLOCATED;
			return node;
		}

		if (node->prev_node_offset == NULL_OFFSET)
			break;

		node = offset_to_pointer<HeapNode>(heap_base, node->prev_node_offset);
	}

	return nullptr;
}

void heap_free(Heap *heap, HeapNode *node)
{
	assert(check_fourcc(node->magic, "memz"));
	assert(node->flags & HEAP_FLAG_ALLOCATED);

	void *heap_base = offset_to_pointer<void>(heap, heap->buffer_offset);
	uint32_t capacity = heap->size - heap->buffer_offset;

	uint32_t node_real_next = node->next_node_offset == NULL_OFFSET ? capacity : node->next_node_offset;
	uint32_t node_real_size = node_real_next - pointer_to_offset(heap_base, node);
	assert(node_real_size <= heap->buffer_usage);

	node->flags &= ~HEAP_FLAG_ALLOCATED;
	heap->buffer_usage -= node_real_size;

	// Forward scan.
	while (node->next_node_offset != NULL_OFFSET) {
		HeapNode *next = offset_to_pointer<HeapNode>(heap_base, node->next_node_offset);
		assert(check_fourcc(next->magic, "memz"));

		if (next->flags & HEAP_FLAG_ALLOCATED)
			break;

		node->next_node_offset = next->next_node_offset;
		std::memset(next->magic, 0, sizeof(next->magic));
	}

	// Reverse scan.
	while (node->prev_node_offset != NULL_OFFSET) {
		HeapNode *prev = offset_to_pointer<HeapNode>(heap_base, node->prev_node_offset);
		assert(check_fourcc(prev->magic, "memz"));

		if (prev->flags & HEAP_FLAG_ALLOCATED)
			break;

		prev->next_node_offset = node->next_node_offset;
		std::memset(node->magic, 0, sizeof(node->magic));
		node = prev;
	}

	heap->last_free_offset = pointer_to_offset(heap_base, node);
}

} // namespace ipc
