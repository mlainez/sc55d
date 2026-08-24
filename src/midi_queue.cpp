#include "midi_queue.h"

#include "core.h"

void MidiQueue::Push(const uint8_t *data, size_t length)
{
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);

    /* The whole ring is usable.  head_ and tail_ are monotonic counters, not
     * masked indices, so a full ring is head - tail == kSize and an empty one
     * is head == tail: the two cannot be confused, and there is no need to
     * sacrifice a slot to tell them apart. */
    const uint32_t free_bytes = kSize - (head - tail);
    if (length > free_bytes)
    {
        dropped_.fetch_add((unsigned long)length, std::memory_order_relaxed);
        return;
    }

    for (size_t i = 0; i < length; i++)
        data_[(head + i) & kMask] = data[i];

    head_.store((uint32_t)(head + length), std::memory_order_release);
}

size_t MidiQueue::Drain()
{
    const uint32_t tail = tail_.load(std::memory_order_relaxed);

    /* Acquire, rather than the cheaper relaxed load plus an acquire fence:
     * this runs a handful of times per period, so the barrier costs nothing
     * measurable, and ThreadSanitizer does not model standalone fences.  It
     * reports the fence version as a race, which would make TSan useless on
     * the one file in sc55d where it has something to say. */
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (head == tail)
        return 0;

    const uint32_t count = head - tail;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t byte = data_[(tail + i) & kMask];
        Core::PostMidi(&byte, 1);
    }

    tail_.store(head, std::memory_order_release);
    return count;
}
