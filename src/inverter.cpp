#include "inverter.hpp"

#include "battery.hpp"
#include "pylontech.hpp"
#include "shared_state.hpp"

#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace {

constexpr size_t kRxBufLimit      = 4096;
constexpr int    kReopenBackoffMs = 5000;

// Reserve batteries advertise a 0.5C charge/discharge current limit derived
// from their own configured capacity — that is the whole reason a reserve is
// exposed (it lifts the inverter's summed current budget).
constexpr int kReserveCRateTenths = 5;   // 0.5C, in tenths

}  // namespace

Inverter::Inverter(std::string device, int baud,
                   const std::vector<std::unique_ptr<Battery>>& batteries)
    : batteries_(batteries),
      transport_(std::move(device), baud, kReopenBackoffMs) {
    build_address_map();
}

Inverter::~Inverter() = default;

void Inverter::build_address_map() {
    uint8_t adr = kBaseAddress;
    for (const auto& b : batteries_) {
        Battery* mirror = nullptr;
        if (!b->monitored()) {
            const std::string& want = b->config().mirror;
            auto it = std::find_if(batteries_.begin(), batteries_.end(),
                [&](const std::unique_ptr<Battery>& o) {
                    return o->name() == want;
                });
            mirror = (it != batteries_.end()) ? it->get() : nullptr;
        }

        const uint32_t cap_mah =
            static_cast<uint32_t>(b->config().capacity) * 1000;
        const uint8_t n = module_count_for_capacity(cap_mah);
        for (uint8_t m = 0; m < n; ++m)
            slots_.push_back(Slot{b.get(), mirror, n});

        spdlog::info("inverter: battery '{}'{} -> {} module(s), ADR {}..{}",
                     b->name(),
                     mirror ? fmt::format(" (reserve, mirrors '{}')",
                                          mirror->name()) : "",
                     n, adr, adr + n - 1);
        adr = static_cast<uint8_t>(adr + n);
    }
}

void Inverter::attach(Dispatcher& d) {
    transport_.start(d, *this);
}

void Inverter::on_connected() {
    spdlog::info("inverter: UART up");
}

void Inverter::on_disconnected() {
    spdlog::warn("inverter: UART down");
}

bool Inverter::send_response(const std::vector<uint8_t>& bytes) {
    return transport_.send(bytes.data(), bytes.size());
}

void Inverter::on_bytes(const uint8_t* data, size_t len) {
    spdlog::trace("inverter: rx {} bytes: {:Xpn}", len,
                  spdlog::to_hex(data, data + len));

    if (rx_buf_.size() + len > kRxBufLimit) {
        spdlog::warn("inverter: rx buffer overflow, dropping accumulated {} bytes",
                     rx_buf_.size());
        rx_buf_.clear();
    }
    rx_buf_.insert(rx_buf_.end(), data, data + len);
    process_buffer();
}

void Inverter::process_buffer() {
    while (!rx_buf_.empty()) {
        PylontechFrame f;
        int n = try_parse_pylontech(rx_buf_.data(), rx_buf_.size(), f);
        if (n == 0) return;
        if (n < 0) {
            size_t drop = static_cast<size_t>(-n);
            if (drop > rx_buf_.size()) drop = rx_buf_.size();
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + drop);
            continue;
        }
        rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + n);

        if (f.adr < kBaseAddress ||
            static_cast<size_t>(f.adr - kBaseAddress) >= slots_.size()) {
            spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} (not us)",
                          f.cid2, f.adr);
            continue;
        }
        const Slot&   slot         = slots_[f.adr - kBaseAddress];
        const bool    is_reserve   = slot.mirror != nullptr;
        Battery*      src          = is_reserve ? slot.mirror : slot.battery;
        const uint8_t module_count = slot.module_count;

        const auto snap = src->state().snapshot();
        const bool have_live = src->state().is_fresh(std::chrono::seconds(5))
                               && snap.cells.has_value()
                               && snap.pack.has_value();
        if (!have_live || !snap.settings.has_value()) {
            spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} '{}' not "
                          "ready (live={} settings={}), silent",
                          f.cid2, f.adr, slot.battery->name(), have_live,
                          snap.settings.has_value());
            continue;
        }

        JkCellInfo cells = *snap.cells;
        JkPackInfo pack  = *snap.pack;
        JkSettings lim   = *snap.settings;

        if (is_reserve) {
            const uint32_t cap_mah =
                static_cast<uint32_t>(slot.battery->config().capacity) * 1000;
            const uint32_t ilim_ma =
                static_cast<uint32_t>(slot.battery->config().capacity)
                * 1000 * kReserveCRateTenths / 10;

            pack.state_of_charge_pct    = 100;
            pack.current_ma             = 0;
            pack.remaining_capacity_mah = cap_mah;
            pack.total_capacity_mah     = cap_mah;
            lim.max_charge_current_ma    = ilim_ma;
            lim.max_discharge_current_ma = ilim_ma;
        }

        // Per-module slice: cells are shared as-is (parallel modules);
        // pack-side aggregates and the current limits get divided by the
        // battery's module count so the inverter sums them back.
        pack.current_ma             /= module_count;
        pack.remaining_capacity_mah /= module_count;
        pack.total_capacity_mah     /= module_count;
        lim.max_charge_current_ma    /= module_count;
        lim.max_discharge_current_ma /= module_count;

        std::vector<uint8_t> resp;
        switch (f.cid2) {
            case 0x42: resp = build_analog_response(f, cells, pack); break;
            case 0x44: resp = build_alarm_response(f, cells);        break;
            case 0x47: resp = build_system_param_response(f, lim);   break;
            case 0x92: resp = build_chgmgmt_response(f, pack, lim);  break;
            default:
                spdlog::warn("inverter: unsupported CID2={:#04x}, no response",
                             f.cid2);
                continue;
        }

        spdlog::debug("inverter: req CID2={:#04x} ADR={:#04x} '{}' -> {} bytes",
                      f.cid2, f.adr, slot.battery->name(), resp.size());
        spdlog::trace("inverter: tx: {:Xpn}", spdlog::to_hex(resp));
        send_response(resp);
    }
}
