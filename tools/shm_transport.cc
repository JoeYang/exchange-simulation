#include "tools/shm_transport.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

namespace exchange {

// ---------------------------------------------------------------------------
// ShmProducer
// ---------------------------------------------------------------------------

ShmProducer::ShmProducer(const std::string& shm_name) : shm_name_(shm_name) {
    // O_CREAT | O_RDWR: create if absent, open read/write.
    // O_TRUNC is intentionally omitted; we use ftruncate to set the exact size
    // and placement-new to reinitialise the ring buffer below.
    fd_ = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ == -1) {
        throw std::runtime_error(
            std::string("ShmProducer: shm_open failed for '") + shm_name +
            "': " + std::strerror(errno));
    }

    if (::ftruncate(fd_, static_cast<off_t>(sizeof(ShmRingBuffer))) == -1) {
        const int saved = errno;
        ::close(fd_);
        ::shm_unlink(shm_name.c_str());
        throw std::runtime_error(
            std::string("ShmProducer: ftruncate failed for '") + shm_name +
            "': " + std::strerror(saved));
    }

    mapped_ = ::mmap(nullptr, sizeof(ShmRingBuffer),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        const int saved = errno;
        ::close(fd_);
        ::shm_unlink(shm_name.c_str());
        throw std::runtime_error(
            std::string("ShmProducer: mmap failed for '") + shm_name +
            "': " + std::strerror(saved));
    }

    // Placement-new initialises the ring buffer in the shared mapping.
    // The producer is the sole writer to write_pos_; the consumer updates
    // read_pos_ on its side.
    ring_ = new (mapped_) ShmRingBuffer();
}

ShmProducer::~ShmProducer() {
    if (mapped_ != nullptr && mapped_ != MAP_FAILED) {
        ::munmap(mapped_, sizeof(ShmRingBuffer));
        mapped_ = nullptr;
        ring_ = nullptr;
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
    // Producer owns the segment name; remove it so no new consumers can open it.
    if (!shm_name_.empty()) {
        ::shm_unlink(shm_name_.c_str());
    }
}

bool ShmProducer::publish(const RecordedEvent& event) {
    return ring_->try_push(event);
}

// ---------------------------------------------------------------------------
// ShmConsumer
// ---------------------------------------------------------------------------

ShmConsumer::ShmConsumer(const std::string& shm_name) : shm_name_(shm_name) {
    // O_RDWR: consumer needs write access for read_pos_ atomic updates inside
    // the ring buffer even though it never writes event data.
    fd_ = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    if (fd_ == -1) {
        throw std::runtime_error(
            std::string("ShmConsumer: shm_open failed for '") + shm_name +
            "': " + std::strerror(errno));
    }

    mapped_ = ::mmap(nullptr, sizeof(ShmRingBuffer),
                     PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        const int saved = errno;
        ::close(fd_);
        throw std::runtime_error(
            std::string("ShmConsumer: mmap failed for '") + shm_name +
            "': " + std::strerror(saved));
    }

    ring_ = reinterpret_cast<ShmRingBuffer*>(mapped_);
}

ShmConsumer::~ShmConsumer() {
    if (mapped_ != nullptr && mapped_ != MAP_FAILED) {
        ::munmap(mapped_, sizeof(ShmRingBuffer));
        mapped_ = nullptr;
        ring_ = nullptr;
    }
    if (fd_ != -1) {
        ::close(fd_);
        fd_ = -1;
    }
    // Consumer does NOT unlink — the producer owns the segment lifetime.
}

bool ShmConsumer::poll(RecordedEvent& event) {
    return ring_->try_pop(event);
}

}  // namespace exchange
