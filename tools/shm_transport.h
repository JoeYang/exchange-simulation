#pragma once
#include "exchange-core/spsc_ring_buffer.h"
#include "test-harness/recorded_event.h"
#include <string>

namespace exchange {

// Ring buffer type for shared memory transport.
// RecordedEvent must be trivially copyable; all member event types contain
// only integer and enum fields so the variant satisfies this requirement.
static_assert(std::is_trivially_copyable_v<RecordedEvent>,
              "RecordedEvent must be trivially copyable for shared memory transport");

using ShmRingBuffer = SpscRingBuffer<RecordedEvent, 65536>;

// ShmProducer creates a POSIX shared memory segment, placement-news a
// ShmRingBuffer into it, and exposes try_push via publish().
//
// Lifetime: the producer owns the segment — the destructor calls shm_unlink,
// removing the name from the filesystem so no new consumers can attach.
// Existing mappings in consumer processes remain valid until those consumers
// unmap.
//
// Non-copyable and non-movable: the ring buffer lives at a fixed virtual
// address; moving the wrapper would leave ring_ dangling.
class ShmProducer {
    int fd_{-1};
    void* mapped_{nullptr};
    ShmRingBuffer* ring_{nullptr};
    std::string shm_name_;

public:
    // Creates (or recreates) the shared memory segment named shm_name.
    // shm_name must start with '/' per POSIX (e.g. "/exchange_events").
    // Throws std::runtime_error on any OS-level failure.
    explicit ShmProducer(const std::string& shm_name);
    ~ShmProducer();

    ShmProducer(const ShmProducer&) = delete;
    ShmProducer& operator=(const ShmProducer&) = delete;

    // Attempt to enqueue event into the ring buffer.
    // Returns false if the buffer is full (caller must back-pressure or drop).
    bool publish(const RecordedEvent& event);

    const std::string& name() const { return shm_name_; }
};

// ShmConsumer attaches to an existing POSIX shared memory segment created by
// ShmProducer and exposes try_pop via poll().
//
// The consumer does NOT unlink the segment on destruction — that is the
// producer's responsibility.
//
// Non-copyable and non-movable for the same reason as ShmProducer.
class ShmConsumer {
    int fd_{-1};
    void* mapped_{nullptr};
    ShmRingBuffer* ring_{nullptr};
    std::string shm_name_;

public:
    // Attaches to the shared memory segment named shm_name.
    // The segment must already exist (producer must be running).
    // Throws std::runtime_error if shm_open fails (e.g. no producer).
    explicit ShmConsumer(const std::string& shm_name);
    ~ShmConsumer();

    ShmConsumer(const ShmConsumer&) = delete;
    ShmConsumer& operator=(const ShmConsumer&) = delete;

    // Attempt to dequeue one event from the ring buffer.
    // Returns false if the buffer is empty.
    bool poll(RecordedEvent& event);

    const std::string& name() const { return shm_name_; }
};

}  // namespace exchange
