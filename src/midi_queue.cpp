#include "midi_queue.h"

#include "core.h"

void MidiQueue::Push(const uint8_t *data, size_t length)
{
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);

    /* The whole ring is usable.  head_ and tail_ are monotonic counters, not
     * masked indices, so a full ring is head - tail == kSize and an empty one
     * is head == tail: the two cannot be confused, and there is no need to
     * sacrifice a slot to tell them apart.
     *
     * That last byte is not academic.  midi_in.cpp decodes into a buffer of
     * exactly kSize, so giving up a slot here would mean a maximal sysex that
     * ALSA decoded successfully could never be pushed -- refused every time,
     * even into a completely empty queue.
     *
     * The invariant this rests on is 0 <= head - tail <= kSize, and it is a
     * sharp edge: let `used` exceed kSize just once and this subtraction
     * underflows to about four billion, after which the guard below accepts
     * everything.  tests/midi_queue has a mutant for exactly that. */
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

    /* Acquire, rather than the cheaper relaxed load plus an acquire fence.
     *
     * The fence version is correct -- [atomics.fences]/3: a release store
     * synchronizes with an acquire fence when an atomic read of the same
     * object, sequenced before the fence, reads the value it wrote.  It is
     * ThreadSanitizer that cannot see it, and not by accident: GCC warns
     * "'atomic_thread_fence' is not supported with '-fsanitize=thread'"
     * (-Wtsan), and TSan duly reports the payload read as a race.
     *
     * This runs a handful of times per period, not once per emulated
     * instruction, so the barrier costs nothing measurable -- and giving it up
     * would cost a usable TSan on the one file in sc55d where TSan has
     * anything to say.  Cheap trade. */
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
