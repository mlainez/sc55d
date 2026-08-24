#include "ring.h"

#include <chrono>

void PeriodRing::Init(unsigned slots, unsigned period_frames)
{
    slots_ = slots;
    stride_ = period_frames * 2;
    data_.assign((size_t)slots_ * stride_, 0);
    head_ = 0;
    tail_ = 0;
    done_ = false;
    starves_ = 0;
    min_fill_ = slots_;
}

int16_t *PeriodRing::BeginWrite()
{
    std::unique_lock<std::mutex> lock(mutex_);
    writable_.wait(lock, [this] { return done_ || head_ - tail_ < slots_; });
    if (done_)
        return nullptr;
    return data_.data() + (size_t)(head_ % slots_) * stride_;
}

void PeriodRing::CommitWrite()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        head_++;
    }
    readable_.notify_one();
}

PeriodRing::Fill PeriodRing::WaitPrefilled(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    readable_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return done_ || head_ - tail_ >= slots_; });
    if (done_)
        return Fill::Closed;
    return head_ - tail_ >= slots_ ? Fill::Ready : Fill::TimedOut;
}

const int16_t *PeriodRing::BeginRead()
{
    std::unique_lock<std::mutex> lock(mutex_);

    /* Before the counters, not after: an empty ring at shutdown is the loop
     * ending, not the renderer failing to keep up, and recording it as a
     * starve puts a "the core is not sustaining realtime" warning on the end
     * of a perfectly clean run. */
    if (done_)
        return nullptr;

    const unsigned fill = (unsigned)(head_ - tail_);
    if (fill < min_fill_)
        min_fill_ = fill;

    if (fill == 0)
    {
        /* This one is real: the ring was empty while the renderer was still
         * running, so the output thread had to wait for it. */
        starves_++;
        readable_.wait(lock, [this] { return done_ || head_ != tail_; });
        if (done_)
            return nullptr;
    }
    return data_.data() + (size_t)(tail_ % slots_) * stride_;
}

void PeriodRing::CommitRead()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tail_++;
    }
    writable_.notify_one();
}

void PeriodRing::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
    }
    writable_.notify_all();
    readable_.notify_all();
}

unsigned long PeriodRing::Starves() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return starves_;
}

unsigned PeriodRing::MinFill() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return min_fill_;
}
