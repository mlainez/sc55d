// SPDX-License-Identifier: MIT

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

/*
 * A single-producer/single-consumer ring of fixed-size audio periods.
 *
 * This is what buys sc55d a second core.  Without it, rendering a period and
 * writing the previous one are the same serial loop, so the emulator only gets
 * to run in the gaps snd_pcm_writei() leaves it: a late wake-up, a page fault
 * or a burst of expensive instructions comes straight out of the audio
 * deadline.  With it, the render thread runs continuously on one core while
 * the output thread blocks in ALSA on another, and the filled slots in between
 * absorb the jitter.
 *
 * It does not make the emulator faster.  If the core cannot sustain realtime
 * on average, the ring drains and the starve counter climbs -- that is exactly
 * the distinction the counters below are there to draw.
 *
 * Slot indices are monotonic counters, so full and empty never alias.  The
 * mutex guards only those counters; the audio itself is written and read
 * outside it, because the producer and the consumer never hold the same slot.
 */
class PeriodRing {
public:
    /* `slots` must be at least 1; --render-ahead 0 does not build a ring at
     * all, it takes the single-threaded path instead. */
    void Init(unsigned slots, unsigned period_frames);

    /* Blocks until a slot is free.  Returns nullptr once Shutdown() has run. */
    int16_t *BeginWrite();
    void CommitWrite();

    enum class Fill {
        Ready,    /* the ring is full; playback can start */
        TimedOut, /* still filling -- caller should check its own quit flag */
        Closed,   /* Shutdown() ran; the renderer is gone */
    };

    /* Blocks until the ring holds `slots` periods, so playback starts from a
     * full buffer rather than racing the renderer.  Times out so a caller
     * waiting here can still notice a signal. */
    Fill WaitPrefilled(int timeout_ms);

    /* Blocks until a period is available.  Returns nullptr on shutdown. */
    const int16_t *BeginRead();
    void CommitRead();

    /* Wakes both sides so they can leave their loops. */
    void Shutdown();

    /* Times the output thread found the ring empty and had to wait for the
     * renderer.  Non-zero means the core is not sustaining realtime; each one
     * is an xrun unless the ALSA buffer covered it. */
    unsigned long Starves() const;

    /* Fewest periods the output thread ever found queued.  How much headroom
     * was left at the worst moment of the run -- 0 with no starves means it
     * came down to the last period. */
    unsigned MinFill() const;

    unsigned Slots() const { return slots_; }

private:
    std::vector<int16_t> data_;
    unsigned slots_ = 0;
    unsigned stride_ = 0; /* int16_t per slot */
    uint64_t head_ = 0;   /* periods produced */
    uint64_t tail_ = 0;   /* periods consumed */
    bool done_ = false;
    unsigned long starves_ = 0;
    unsigned min_fill_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable writable_;
    std::condition_variable readable_;
};
