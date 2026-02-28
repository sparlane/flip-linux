#pragma once
#include <map>
#include <memory>
#include "flip_proto.hpp"
#include "netdrv.hpp"
#include "rpc_port_manager.hpp"

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
    std::shared_ptr<RpcPortManager> rpc_port_mgr;
    std::shared_ptr<flip_networks> networks;
    std::shared_ptr<flip_route_entry> find_route(flip_address_t dst);
    void handle_rpc_locate(flip_address_t src_addr, flip_address_t dst_addr, const rpc_header* rpc_hdr, uint16_t actual_hopcount, const uint8_t* payload, size_t payload_len, flip_network_t incoming_network);
    void handle_rpc_hereis(flip_address_t src_addr, const rpc_header* rpc_hdr);
    void forward_broadcast(const uint8_t* packet, size_t len, flip_network_t incoming_network);
    void forward_unicast(const uint8_t* packet, size_t len, const hwaddr_t dst_mac, flip_network_t dst_network);
public:
    flip_router(std::shared_ptr<flip_networks> net);
    ~flip_router();
    void route_packet(hwaddr_t src_mac, const uint8_t* packet, size_t len, flip_network_t incoming_network);
    void increment_age();
    bool install_local_address(flip_address_t address);
    void remove_local_address(flip_address_t address);
    std::shared_ptr<RpcPortManager> get_rpc_port_manager() { return rpc_port_mgr; }
};
