#include <iostream>
#include <cstring>
#include "flip_router.hpp"

flip_router::flip_router(std::shared_ptr<flip_networks> net)
{
    rpc_port_mgr = std::make_shared<RpcPortManager>();
    networks = net;
}

flip_router::~flip_router()
{
    // Cleanup resources if needed
}

std::shared_ptr<flip_route_entry> flip_router::find_route(flip_address_t dst)
{
    auto it = routing_table.find(dst);
    if (it != routing_table.end()) {
        return it->second;
    }
    return nullptr;
}

static std::string packet_type_to_string(flip_type type) {
    switch (type) {
        case flip_type::LOCATE: return "LOCATE";
        case flip_type::HEREIS: return "HEREIS";
        case flip_type::UNIDATA: return "UNIDATA";
        case flip_type::MULTIDATA: return "MULTIDATA";
        case flip_type::NOTHERE: return "NOTHERE";
        case flip_type::UNTRUSTED: return "UNTRUSTED";
        default: return "UNKNOWN";
    }
}

void flip_router::route_packet(hwaddr_t src_mac, const uint8_t* packet, size_t len, flip_network_t incoming_network)
{
    if (len < sizeof(struct flip_packet)) {
        std::cerr << "Received packet too short for FLIP header" << std::endl;
        return;
    }
    const struct flip_packet* fp = (const struct flip_packet*)packet;

    if (fp->src_address != 0) {
        // Update routing table with source address and incoming network
        auto route = this->find_route(fp->src_address);
        if (!route || (fp->flags & FLIP_FLAG_UNSAFE && route->hopcount > fp->actual_hopcount)) {
            route = std::make_shared<flip_route_entry>();
            route->dst_address = fp->src_address;
            route->network = incoming_network;
            route->next_hop_mac = src_mac;
            route->hopcount = fp->actual_hopcount;
            route->trusted = (fp->flags & FLIP_FLAG_SECURITY) != 0;
            route->age = 0;
            route->local = false;
            routing_table[fp->src_address] = route;
            std::cout << "Added route for " << fp->src_address << " via network " << incoming_network << std::endl;
        }
    }

    std::shared_ptr<flip_route_entry> dst_route = nullptr;
    if (fp->dst_address != 0) {
        dst_route = this->find_route(fp->dst_address);
    }

    std::cout << "Received " << (int)fp->type << " packet from " << (int)src_mac[0] << ":" << (int)src_mac[1] << ":" << (int)src_mac[2] << ":" << (int)src_mac[3] << ":" << (int)src_mac[4] << ":" << (int)src_mac[5] << std::endl;

    switch ((flip_type)fp->type)
    {
        case flip_type::LOCATE:
            if (fp->actual_hopcount == fp->max_hopcount && dst_route && dst_route->local) {
                std::cout << "Destination " << fp->dst_address << " is local, sending HEREIS response" << std::endl;
                // Send HEREIS response back to src_mac
            } else if (!dst_route || !dst_route->local) {
                // Forward LOCATE packet to all other networks if destination not found or not local
                if (fp->actual_hopcount < fp->max_hopcount) {
                    forward_broadcast(packet, len, incoming_network);
                }
            }
            break;
        case flip_type::HEREIS:
            // Route already added by the source based route adding.
            // May need to forward this packet to nodes that requested this address
            if (dst_route && !dst_route->local && dst_route->network != incoming_network) {
                forward_unicast(packet, len, dst_route->next_hop_mac, dst_route->network);
            }
            break;
        case flip_type::MULTIDATA:
            // MULTIDATA packets have a 4-byte proto field followed by protocol-specific data
            if (len >= sizeof(flip_packet) + sizeof(uint32_t) + sizeof(rpc_header)) {
                uint32_t proto;
                std::memcpy(&proto, packet + sizeof(struct flip_packet), sizeof(uint32_t));
                proto = ntohl(proto);
                if (proto == PROTO_RPC) {
                    const rpc_header* rpc_hdr = (const rpc_header*)(packet + sizeof(struct flip_packet) + sizeof(uint32_t));
                    if (rpc_hdr->type == AM_RPC_LOCATE && fp->dst_address == 0) {
                        const uint8_t* payload = packet + sizeof(struct flip_packet) + sizeof(uint32_t) + sizeof(rpc_header);
                        size_t payload_len = len - sizeof(struct flip_packet) - sizeof(uint32_t) - sizeof(rpc_header);
                        handle_rpc_locate(fp->src_address, fp->dst_address, rpc_hdr, fp->actual_hopcount, payload, payload_len, incoming_network);
                    }
                }
            }
            // Forward MULTIDATA as broadcast to all networks except incoming
            if (fp->actual_hopcount < fp->max_hopcount) {
                forward_broadcast(packet, len, incoming_network);
            }
            break;
        case flip_type::UNIDATA:
            // Check if this is an RPC HEREIS message
            if (len >= sizeof(struct flip_packet) + sizeof(rpc_header) && fp->offset == 0) {
                const rpc_header* rpc_hdr = (const rpc_header*)(packet + sizeof(struct flip_packet));
                if (rpc_hdr->type == AM_RPC_HEREIS) {
                    handle_rpc_hereis(fp->src_address, rpc_hdr);
                }
            }

            // Route UNIDATA based on destination
            if (dst_route && dst_route->local) {
                // Destination is local, deliver to application (don't forward)
                std::cout << "UNIDATA for local destination " << fp->dst_address << std::endl;
            } else if (dst_route && fp->actual_hopcount < fp->max_hopcount) {
                // Destination is known, forward to specific network
                forward_unicast(packet, len, dst_route->next_hop_mac, dst_route->network);
            } else if (!dst_route) {
                // Destination unknown - may need to generate implicit LOCATE
                std::cout << "UNIDATA for unknown destination " << fp->dst_address << " (no route)" << std::endl;
            }
            break;
        case flip_type::NOTHERE:
        case flip_type::UNTRUSTED:
            {
                if (dst_route && dst_route->network == incoming_network &&  fp->type == (uint8_t)flip_type::NOTHERE) {
                    std::cout << "Received NOTHERE for destination " << fp->dst_address << " on network " << incoming_network << ", removing route" << std::endl;
                    routing_table.erase(fp->dst_address);
                }
                auto src_route = this->find_route(fp->src_address);
                if (src_route) {
                    if (src_route->network == incoming_network) {
                        // Skip forwarding NOTHERE/UNTRUSTED back to source of original packet if it came from this network
                    }
                    else {
                        forward_unicast(packet, len, src_route->next_hop_mac, src_route->network);
                    }
                }
            }
            break;
        default:
            std::cerr << "Received packet with unknown FLIP type: " << (int)fp->type << std::endl;
            break;
    }
}

void flip_router::increment_age()
{
    // Increment the age of routing entries, remove stale entries, etc.
    // This will be called periodically to maintain the routing table
}

void flip_router::handle_rpc_locate(flip_address_t src_addr, flip_address_t dst_addr, const rpc_header* rpc_hdr, uint16_t actual_hopcount, const uint8_t* payload, size_t payload_len, flip_network_t incoming_network)
{
    (void)payload;
    (void)payload_len;
    (void)incoming_network;

    // Check if we have this port registered locally
    rpc_port_t port_array;
    std::copy(rpc_hdr->port, rpc_hdr->port + 6, port_array.begin());
    auto binding = rpc_port_mgr->get_local_binding(port_array);
    if (!binding.has_value()) {
        std::cout << "RPC LOCATE for port " << (int)rpc_hdr->port[0] << " not found locally" << std::endl;
        return;
    }

    std::cout << "RPC LOCATE for port " << (int)rpc_hdr->port[0] << " found locally, sending HEREIS" << std::endl;

    // Build and send HEREIS response
    struct flip_packet hereis_pkt;
    hereis_pkt.version = 1;
    hereis_pkt.type = (uint8_t)flip_type::UNIDATA;
    hereis_pkt.flags = 0;
    hereis_pkt.reserved = 0;
    hereis_pkt.actual_hopcount = 0;
    hereis_pkt.max_hopcount = actual_hopcount;
    hereis_pkt.dst_address = src_addr;
    hereis_pkt.src_address = dst_addr;
    hereis_pkt.message_id = rpc_hdr->tid;
    hereis_pkt.offset = 0;

    // Create RPC HEREIS response header
    rpc_header hereis_rpc;
    hereis_rpc.kid = rpc_hdr->kid;
    std::copy(rpc_hdr->port, rpc_hdr->port + 6, hereis_rpc.port);
    hereis_rpc.type = AM_RPC_HEREIS;
    hereis_rpc.flags = 0;
    hereis_rpc.tid = rpc_hdr->tid;
    hereis_rpc.dest = rpc_hdr->from;
    hereis_rpc.from = rpc_hdr->dest;

    // Socket name payload
    const std::string& socket_name = binding->unix_socket;
    size_t rpc_payload_len = sizeof(hereis_rpc) + socket_name.size();
    hereis_pkt.length = rpc_payload_len;
    hereis_pkt.total_length = rpc_payload_len;

    uint8_t hereis_buf[512];
    if (sizeof(hereis_pkt) + sizeof(hereis_rpc) + socket_name.size() > sizeof(hereis_buf)) {
        std::cerr << "RPC HEREIS response too large" << std::endl;
        return;
    }

    std::memcpy(hereis_buf, &hereis_pkt, sizeof(hereis_pkt));
    std::memcpy(hereis_buf + sizeof(hereis_pkt), &hereis_rpc, sizeof(hereis_rpc));
    std::memcpy(hereis_buf + sizeof(hereis_pkt) + sizeof(hereis_rpc), socket_name.c_str(), socket_name.size());

    // TODO: Send HEREIS packet through network driver
    std::cout << "HEREIS response prepared (socket: " << socket_name << ")" << std::endl;
}

void flip_router::handle_rpc_hereis(flip_address_t src_addr, const rpc_header* rpc_hdr)
{
    std::cout << "Received RPC HEREIS for port " << (int)rpc_hdr->port[0] << " from " << src_addr << std::endl;
    // Resolve pending lookup with the source address as the remote socket identifier
    rpc_port_t port_array;
    std::copy(rpc_hdr->port, rpc_hdr->port + 6, port_array.begin());
    rpc_port_mgr->resolve_remote_lookup(port_array, std::to_string(src_addr), true);
}

void flip_router::forward_broadcast(const uint8_t* packet, size_t len, flip_network_t incoming_network)
{
    if (!networks) {
        return;
    }

    uint8_t forward_buf[2048];
    if (len > sizeof(forward_buf)) {
        std::cerr << "Packet too large to forward" << std::endl;
        return;
    }

    std::memcpy(forward_buf, packet, len);
    struct flip_packet* forward_pkt = (struct flip_packet*)forward_buf;
    forward_pkt->actual_hopcount++;

    const auto& nets = networks->get_networks();
    for (const auto& [net_id, driver] : nets) {
        if (net_id != incoming_network) {
            if (!driver->send(std::array<uint8_t, 6>{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, flip_ethertype_network(), forward_buf, len)) {
                std::cerr << "Failed to forward " << packet_type_to_string((flip_type)forward_pkt->type) << " to network " << net_id << std::endl;
            } else {
                std::cout << "Forwarded " << packet_type_to_string((flip_type)forward_pkt->type) << " to network " << net_id << std::endl;
            }
        }
    }
}

void flip_router::forward_unicast(const uint8_t* packet, size_t len, const hwaddr_t dst_mac, flip_network_t dst_network)
{
    if (!networks) {
        return;
    }

    uint8_t forward_buf[2048];
    if (len > sizeof(forward_buf)) {
        std::cerr << "Packet too large to forward" << std::endl;
        return;
    }

    std::memcpy(forward_buf, packet, len);
    struct flip_packet* forward_pkt = (struct flip_packet*)forward_buf;
    forward_pkt->actual_hopcount++;

    const auto& nets = networks->get_networks();
    auto it = nets.find(dst_network);
    if (it != nets.end()) {
        if (!it->second->send(dst_mac, flip_ethertype_network(), forward_buf, len)) {
            std::cerr << "Failed to forward " << packet_type_to_string((flip_type)forward_pkt->type) << " to network " << dst_network << std::endl;
        } else {
            std::cout << "Forwarded " << packet_type_to_string((flip_type)forward_pkt->type) << " to network " << dst_network << std::endl;
        }
    }
}