#pragma once

#include "netdrv.hpp"

class Tap : public NetDrv
{
private:
    int fd;
    uint8_t mac[6];
public:
    Tap(const char* dev);
    ~Tap() override;
    bool send(void *buf, size_t len) override;
    int get_fd() const override;
    uint8_t* get_mac() const override;
    ssize_t recv(void* buf, size_t len) override;
};