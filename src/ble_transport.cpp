#include "ble_transport.hpp"

#include "dispatcher.hpp"

#include <utility>

BleTransport::BleTransport(BleClient::Settings settings)
    : ble_(std::move(settings), queue_) {}

BleTransport::~BleTransport() {
    BleTransport::stop();
}

bool BleTransport::send(const uint8_t* data, size_t len) {
    return ble_.send(data, len);
}

void BleTransport::start(Dispatcher& dispatcher, TransportEvents& sink) {
    sink_       = &sink;
    dispatcher_ = &dispatcher;
    dispatcher_->watch(queue_.notify_fd(), [this] { pump(); });
    ble_.start();
}

void BleTransport::stop() {
    ble_.stop();
    if (dispatcher_) {
        dispatcher_->unwatch(queue_.notify_fd());
        dispatcher_ = nullptr;
    }
}

void BleTransport::pump() {
    queue_.drain_notify();
    while (auto ev = queue_.try_pop()) {
        switch (ev->type) {
            case BmsEventType::Connected:    sink_->on_connected();    break;
            case BmsEventType::Disconnected: sink_->on_disconnected(); break;
            case BmsEventType::NotifyFrame:
                sink_->on_bytes(ev->payload.data(), ev->payload.size());
                break;
        }
    }
}
