#include "stream_hub.h"
#include <algorithm>
#include <chrono>

namespace babycam {

void Subscriber::offer(const Packet& p, bool startsHere) {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_) return;
    if (!started_) {
        if (!startsHere) return;       // drop until the first keyframe
        started_ = true;
    }
    if (q_.size() >= maxQueue_) q_.pop_front();   // drop oldest on overflow
    q_.push_back(p);
    cv_.notify_one();
}

Packet Subscriber::next(int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]{ return closed_ || !q_.empty(); });
    if (q_.empty()) return nullptr;
    Packet p = q_.front(); q_.pop_front();
    return p;
}

void Subscriber::close() {
    std::lock_guard<std::mutex> lk(m_);
    closed_ = true; cv_.notify_all();
}

std::shared_ptr<Subscriber> StreamHub::subscribe() {
    auto s = std::make_shared<Subscriber>();
    std::lock_guard<std::mutex> lk(m_);
    subs_.push_back(s);
    return s;
}

void StreamHub::unsubscribe(const std::shared_ptr<Subscriber>& s) {
    std::lock_guard<std::mutex> lk(m_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), s), subs_.end());
}

void StreamHub::publish(const MediaUnit& u) {
    std::vector<uint8_t> local;
    local.reserve(u.data.size() + 1);
    local.push_back(u.kind == MediaUnit::Audio ? 1 : 0);
    local.insert(local.end(), u.data.begin(), u.data.end());
    Packet pkt = std::make_shared<const std::vector<uint8_t>>(std::move(local));
    bool startsHere = (u.kind == MediaUnit::Video && u.keyframe);
    std::vector<std::shared_ptr<Subscriber>> snapshot;
    { std::lock_guard<std::mutex> lk(m_); snapshot = subs_; }
    for (auto& s : snapshot) s->offer(pkt, startsHere);
}

} // namespace babycam
