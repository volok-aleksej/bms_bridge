#pragma once

#include "config.hpp"
#include "shared_state.hpp"

#include <memory>
#include <string>

class BatteryTransport;
class BmsHandler;
class JkRs485Slave;
class History;
class Dispatcher;

class Battery {
public:
    explicit Battery(BatteryConfig cfg);
    ~Battery();

    Battery(const Battery&)            = delete;
    Battery& operator=(const Battery&) = delete;

    const std::string&   name()      const { return cfg_.name; }
    const BatteryConfig& config()    const { return cfg_; }
    SharedState&         state()           { return state_; }
    const SharedState&   state()     const { return state_; }
    bool                 monitored() const { return cfg_.monitored(); }

    // The battery named by config().mirror; set before start().
    void set_mirror(const Battery* m) { mirror_ = m; }

    void start(Dispatcher& dispatcher, History& history);
    void stop();

private:
    void start_relay(Dispatcher& dispatcher);

    BatteryConfig cfg_;
    SharedState   state_;

    std::unique_ptr<BatteryTransport> transport_;
    std::unique_ptr<BmsHandler>       handler_;

    const Battery*                    mirror_ = nullptr;
    std::unique_ptr<JkRs485Slave>  relay_;   // protocol jk_rs485
};
