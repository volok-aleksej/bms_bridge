#include "battery.hpp"

#include "ble_client.hpp"
#include "ble_transport.hpp"
#include "bms_handler.hpp"
#include "dispatcher.hpp"
#include "jk_protocol_driver.hpp"
#include "pylontech_master.hpp"
#include "uart_transport.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <utility>

Battery::Battery(BatteryConfig cfg) : cfg_(std::move(cfg)) {}

Battery::~Battery() = default;

void Battery::start(Dispatcher& dispatcher, History& history) {
    using std::chrono::milliseconds;

    switch (cfg_.link) {
        case BatteryLink::Bluetooth: {
            if (cfg_.protocol != "jk_bt") {
                spdlog::warn("battery '{}': bluetooth protocol '{}' not "
                             "implemented (only jk_bt) — not monitored",
                             cfg_.name, cfg_.protocol);
                return;
            }
            BleClient::Settings s{
                cfg_.address,
                cfg_.service_uuid,
                cfg_.char_write_uuid,
                cfg_.char_notify_uuid,
                cfg_.scan_timeout_ms,
                cfg_.adapter_poll_ms,
                cfg_.reconnect_ms,
            };
            transport_ = std::make_unique<BleTransport>(std::move(s));
            handler_   = std::make_unique<BmsHandler>(
                cfg_.name, std::make_unique<JkProtocol>(), *transport_,
                state_, history,
                milliseconds(cfg_.bms_poll_period_ms),
                milliseconds(cfg_.bms_state_update_interval_ms));

            transport_->start(dispatcher, *handler_);
            dispatcher.add_timer(cfg_.bms_poll_period_ms,
                                 [this] { handler_->on_poll_tick(); });
            dispatcher.add_timer(cfg_.settings_refresh_period_ms,
                                 [this] { handler_->on_settings_tick(); });
            spdlog::info("battery '{}': bluetooth/jk_bt @ {}",
                         cfg_.name, cfg_.address);
            break;
        }
        case BatteryLink::Ttl: {
            if (cfg_.protocol != "pylontech") {
                spdlog::warn("battery '{}': ttl protocol '{}' not "
                             "implemented (only pylontech) — not monitored",
                             cfg_.name, cfg_.protocol);
                return;
            }
            const int wire_adr = cfg_.pylontech_address + 1;
            transport_ = std::make_unique<UartTransport>(
                cfg_.address, cfg_.uart_baud, cfg_.reconnect_ms);
            handler_   = std::make_unique<BmsHandler>(
                cfg_.name,
                std::make_unique<PylontechMasterProtocol>(wire_adr),
                *transport_, state_, history,
                milliseconds(cfg_.bms_poll_period_ms),
                milliseconds(cfg_.bms_state_update_interval_ms));

            transport_->start(dispatcher, *handler_);
            dispatcher.add_timer(cfg_.bms_poll_period_ms,
                                 [this] { handler_->on_poll_tick(); });
            dispatcher.add_timer(cfg_.settings_refresh_period_ms,
                                 [this] { handler_->on_settings_tick(); });
            spdlog::info("battery '{}': ttl/pylontech @ {} adr={}",
                         cfg_.name, cfg_.address, wire_adr);
            break;
        }
        case BatteryLink::Reserve:
            spdlog::info("battery '{}': reserve (capacity {}) — not monitored",
                         cfg_.name, cfg_.capacity);
            break;
    }
}

void Battery::stop() {
    if (transport_) transport_->stop();
}
