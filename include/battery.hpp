#pragma once

#include "config.hpp"
#include "shared_state.hpp"

#include <memory>
#include <string>

class BatteryTransport;
class BmsHandler;
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

    void start(Dispatcher& dispatcher, History& history);
    void stop();

private:
    BatteryConfig cfg_;
    SharedState   state_;

    std::unique_ptr<BatteryTransport> transport_;
    std::unique_ptr<BmsHandler>       handler_;
};
