#pragma once
#include "flip_proto.hpp"
#include "netdrv.hpp"

class flip_route_entry
{
public:
    flip_address_t dst_address;
    flip_network_t network;
    hwaddr_t next_hop_mac;
    uint16_t hopcount;
    bool trusted;
    uint16_t age;
    bool local;
};

class flip_router
{
public:
    flip_router();
    ~flip_router();
};