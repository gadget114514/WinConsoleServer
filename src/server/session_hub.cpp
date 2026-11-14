#include "session_hub.h"

#include "session.h"

#include <vector>

void SessionHub::Register(const std::shared_ptr<Session>& session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->Id()] = session;
}

void SessionHub::Unregister(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

void SessionHub::NotifyPeers(int job_id, uint32_t from_id, uint16_t cols,
                             uint16_t rows) {
    if (job_id <= 0) return;

    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : sessions_) {
            std::shared_ptr<Session> session = kv.second.lock();
            if (session && session->Id() != from_id &&
                session->JobId() == job_id) {
                targets.push_back(std::move(session));
            }
        }
    }

    // Enqueue outside the hub lock: each session takes its own queue lock.
    for (const auto& session : targets) {
        session->NotifyPeerResize(cols, rows);
    }
}
