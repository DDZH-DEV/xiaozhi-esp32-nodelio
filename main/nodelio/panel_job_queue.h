#pragma once

#include <array>
#include <cstring>

namespace nodelio {

// Caller serializes access. Ownership transfers only after a successful Push;
// Pop/replacement transfers ownership back to the caller for run/discard.
struct PanelJob {
    void (*run)(void*) = nullptr;
    void (*discard)(void*) = nullptr;
    void* argument = nullptr;
    unsigned view_epoch = 0;
    bool execute_when_stale = false;
    char key[96]{};
};

class PanelJobQueue {
public:
    static constexpr unsigned kCapacity = 8;
    bool Push(const PanelJob& job, PanelJob& replaced) {
        replaced = {};
        if (job.key[0]) {
            for (unsigned i = 0; i < size_; ++i) {
                auto& queued = jobs_[(head_ + i) % kCapacity];
                if (std::strcmp(queued.key, job.key) == 0) {
                    replaced = queued;
                    queued = job;
                    return true;
                }
            }
        }
        if (size_ == kCapacity) {
            return false;
        }
        jobs_[(head_ + size_++) % kCapacity] = job;
        return true;
    }
    bool Pop(PanelJob& job) {
        if (!size_) {
            return false;
        }
        job = jobs_[head_];
        jobs_[head_] = {};
        head_ = (head_ + 1) % kCapacity;
        --size_;
        return true;
    }

private:
    std::array<PanelJob, kCapacity> jobs_{};
    unsigned head_ = 0;
    unsigned size_ = 0;
};
}  // namespace nodelio
