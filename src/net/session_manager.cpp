#include "net/session_manager.h"

#include "net/session.h"

namespace arena::net {

void SessionManager::add(std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lk(mutex_);
    sessions_.emplace(session->id(), std::move(session));
}

void SessionManager::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    sessions_.erase(id);
}

std::vector<std::shared_ptr<Session>> SessionManager::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::shared_ptr<Session>> out;
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_) {
        out.push_back(kv.second);
    }
    return out;
}

std::size_t SessionManager::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sessions_.size();
}

}  // namespace arena::net
