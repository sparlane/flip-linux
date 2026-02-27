#pragma once
#include <map>
#include <memory>
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
private:
    std::map<flip_address_t, std::shared_ptr<flip_route_entry>> routing_table;
    std::shared_ptr<flip_route_entry> find_route(flip_address_t dst);
public:
    flip_router();
    ~flip_router();
    void route_packet(hwaddr_t src_mac, const uint8_t* packet, size_t len, flip_network_t incoming_network);
    void increment_age();
};