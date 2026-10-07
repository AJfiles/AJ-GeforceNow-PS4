#include "network_monitor.hpp"

#include <algorithm>
#include <utility>

namespace opennow {

NetworkMonitor::NetworkMonitor(
    std::function<bool()> connection_query,
    std::function<NetworkConnectionInfo()> info_query,
    std::chrono::milliseconds connection_interval,
    std::chrono::milliseconds info_interval)
    : connection_query_(std::move(connection_query)),
      info_query_(std::move(info_query)),
      connection_interval_(std::max(connection_interval, std::chrono::milliseconds(1))),
      info_interval_(std::max(info_interval, std::chrono::milliseconds(1))) {
}

NetworkMonitor::~NetworkMonitor() {
    stop();
}

void NetworkMonitor::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable())
        return;
    snapshot_ = {};
    stop_requested_ = false;
    worker_ = std::thread(&NetworkMonitor::run, this);
}

void NetworkMonitor::request_stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
}

void NetworkMonitor::stop() {
    request_stop();
    if (worker_.joinable())
        worker_.join();
}

NetworkMonitorSnapshot NetworkMonitor::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void NetworkMonitor::run() {
    auto next_connection_query = std::chrono::steady_clock::now();
    auto next_info_query = next_connection_query;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_requested_) {
        if (std::chrono::steady_clock::now() >= next_connection_query) {
            lock.unlock();
            const bool connected = connection_query_();
            lock.lock();
            if (stop_requested_)
                break;
            snapshot_.internet_connected = connected;
            next_connection_query = std::chrono::steady_clock::now() + connection_interval_;
        }
        if (std::chrono::steady_clock::now() >= next_info_query) {
            lock.unlock();
            const NetworkConnectionInfo info = info_query_();
            lock.lock();
            if (stop_requested_)
                break;
            snapshot_.connection_info = info;
            next_info_query = std::chrono::steady_clock::now() + info_interval_;
        }
        const auto deadline = std::min(next_connection_query, next_info_query);
        const auto now = std::chrono::steady_clock::now();
        if (deadline > now) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            lock.unlock();
            // Avoid timed condition_variable waits on Orbis: libc++/pthread
            // can report an ordinary timeout as an exception and terminate a
            // background thread. Poll stop at 10 ms granularity instead.
            std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(10)));
            lock.lock();
        }
    }
}

} // namespace opennow
