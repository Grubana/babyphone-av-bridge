#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include "media.h"

namespace babycam {

// A WS-ready payload: 1 tag byte (0=video,1=audio) + media bytes.
using Packet = std::shared_ptr<const std::vector<uint8_t>>;

class Subscriber {
public:
    explicit Subscriber(size_t maxQueue = 256) : maxQueue_(maxQueue) {}
    void offer(const Packet& p, bool startsHere);   // called by hub under its lock
    Packet next(int timeoutMs);                      // blocks; nullptr on timeout
    void close();
private:
    friend class StreamHub;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Packet> q_;
    size_t maxQueue_;
    bool started_ = false;
    bool closed_ = false;
};

class StreamHub {
public:
    std::shared_ptr<Subscriber> subscribe();
    void unsubscribe(const std::shared_ptr<Subscriber>& s);
    void publish(const MediaUnit& u);   // called by the tap thread
private:
    std::mutex m_;
    std::vector<std::shared_ptr<Subscriber>> subs_;
};

} // namespace babycam
