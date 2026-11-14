#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

class Session;

// Tracks live WCSP connections so a resize of a shared job can be forwarded to
// the other clients attached to the same job (FeatureSizeSync).
class SessionHub {
public:
    void Register(const std::shared_ptr<Session>& session);
    void Unregister(uint32_t id);
    void NotifyPeers(int job_id, uint32_t from_id, uint16_t cols, uint16_t rows);

private:
    std::mutex mutex_;
    std::unordered_map<uint32_t, std::weak_ptr<Session>> sessions_;
};
