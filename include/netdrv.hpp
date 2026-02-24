#pragma once
#include <cstdint>
#include <sys/types.h>

class NetDrv
{
public:
    virtual ~NetDrv() = default;
    virtual bool send(void *buf, size_t len) = 0;
    virtual int get_fd() const = 0;
    virtual ssize_t recv(void* buf, size_t len) = 0;
    virtual uint8_t* get_mac() const = 0;
};