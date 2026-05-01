#pragma once

class Event {
public:
    Event();
    ~Event();
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&)                 = delete;
    Event& operator=(Event&&)      = delete;

    int  fd() const noexcept { return fd_; }
    void signal();
    void consume();

private:
    int fd_ = -1;
};
