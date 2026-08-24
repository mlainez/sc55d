#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

/*
 * A lock-free byte queue between the MIDI thread and the render thread.
 *
 * It exists because of what is on the other side of it.  The core's own UART
 * FIFO -- mcu_t::uart_buffer with its uart_write_ptr/uart_read_ptr pair -- is
 * a single-producer ring with no synchronisation whatsoever: MCU_PostUART()
 * stores the byte and then bumps the pointer, both plain stores, and the core
 * polls the pointer and then reads the byte, both plain loads.  Posting to it
 * from a second thread, which is what upstream's RtMidi callback does and
 * what sc55d used to do, is a data race.  On x86 it happens to work.  On a
 * weakly ordered Cortex-A53 the core can see the advanced pointer before the
 * byte it points at and take whatever was in that slot the previous time
 * round the buffer -- a corrupted status byte, which is a stuck note.
 *
 * Fixing that inside the core means making uart_write_ptr atomic, and GCC's
 * AArch64 backend will not fold an atomic load into an offset addressing mode
 * or pair it with the adjacent plain load, so the poll grows three
 * instructions.  It runs about 15 million times per two seconds of audio.
 *
 * So the fix goes here instead: MIDI bytes cross the thread boundary through
 * this queue, and only the render thread ever calls into the core.  The core
 * stays untouched and its UART FIFO becomes what it was always written as --
 * single-threaded.
 */
class MidiQueue {
public:
    /* Producer side: the MIDI thread.  Never blocks.  Drops the message whole
     * rather than partially if there is no room, so the core never sees half
     * a sysex. */
    void Push(const uint8_t *data, size_t length);

    /* Consumer side: the render thread.  Hands every queued byte to
     * Core::PostMidi().  Returns the number of bytes drained. */
    size_t Drain();

    /* Bytes dropped because the queue was full.  Should stay zero: it holds
     * as much as the core's own FIFO does. */
    unsigned long Dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    /* Power of two, and the same 8192 as both the core's own FIFO and
     * midi_in.cpp's decode buffer -- it must not be smaller than the largest
     * message that can be handed to Push(), or that message can never fit. */
    static const uint32_t kSize = 8192;
    static const uint32_t kMask = kSize - 1;

    uint8_t data_[kSize]{};

    /* Written by the producer, read by the consumer, and the other way round.
     * The release/acquire pairing on head_ is what publishes the bytes. */
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<unsigned long> dropped_{0};
};
