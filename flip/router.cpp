#include <iostream>
#include <cstring>
#include <algorithm>
#include "flip_router.hpp"

// Standard Ethernet payload limit (excluding Ethernet header)
constexpr size_t MAX_ETH_PAYLOAD = 1500;
// Max FLIP data bytes per Ethernet frame after fc_header (2B) and flip_packet (40B)
constexpr size_t MAX_FLIP_FRAGMENT_DATA = MAX_ETH_PAYLOAD - sizeof(fc_header) - sizeof(flip_packet);

// Send a FLIP packet (without fc_header), fragmenting across multiple Ethernet frames if needed.
static bool fragment_and_send(std::shared_ptr<NetDrv> driver, const hwaddr_t& dst, uint16_t ethertype,
                               const uint8_t* packet, size_t len)
{
    if (len < sizeof(flip_packet)) return false;

    const flip_packet* orig_fp = reinterpret_cast<const flip_packet*>(packet);
    const uint8_t* payload = packet + sizeof(flip_packet);
    size_t payload_len = len - sizeof(flip_packet);

    if (sizeof(fc_header) + len <= MAX_ETH_PAYLOAD) {
        uint8_t buf[MAX_ETH_PAYLOAD];
        fc_header fch{0, 0};
        std::memcpy(buf, &fch, sizeof(fch));
        std::memcpy(buf + sizeof(fch), packet, len);
        return driver->send(dst, ethertype, buf, sizeof(fch) + len);
    }

    // Packet exceeds Ethernet MTU — fragment the payload
    uint32_t total_length = orig_fp->total_length ? orig_fp->total_length
                                                   : static_cast<uint32_t>(payload_len);
    uint32_t base_offset = orig_fp->offset;
    size_t offset = 0;
    bool ok = true;

    while (offset < payload_len) {
        size_t chunk = std::min(payload_len - offset, MAX_FLIP_FRAGMENT_DATA);

        flip_packet frag_fp = *orig_fp;
        frag_fp.offset      = base_offset + static_cast<uint32_t>(offset);
        frag_fp.length      = static_cast<uint32_t>(chunk);
        frag_fp.total_length = total_length;

        fc_header fch{0, 0};
        uint8_t buf[MAX_ETH_PAYLOAD];
        size_t buf_len = sizeof(fch) + sizeof(frag_fp) + chunk;
        std::memcpy(buf, &fch, sizeof(fch));
        std::memcpy(buf + sizeof(fch), &frag_fp, sizeof(frag_fp));
        std::memcpy(buf + sizeof(fch) + sizeof(frag_fp), payload + offset, chunk);

        if (!driver->send(dst, ethertype, buf, buf_len)) ok = false;
        offset += chunk;

        usleep (1000); // Small delay to avoid overwhelming the network with fragments
    }
    return ok;
}

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
        // Local routes are authoritative; only add/update non-local entries.
        if (!route || (!route->local && (fp->flags & FLIP_FLAG_UNSAFE) && route->hopcount > fp->actual_hopcount)) {
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
                struct flip_packet hereis_pkt{};
                hereis_pkt.version = fp->version;
                hereis_pkt.type = static_cast<uint8_t>(flip_type::HEREIS);
                hereis_pkt.flags = fp->flags;
                hereis_pkt.reserved = 0;
                hereis_pkt.actual_hopcount = 0;
                hereis_pkt.max_hopcount = fp->max_hopcount;
                hereis_pkt.dst_address = fp->src_address;
                hereis_pkt.src_address = fp->dst_address;
                hereis_pkt.message_id = fp->message_id;
                hereis_pkt.length = 0;
                hereis_pkt.offset = 0;
                hereis_pkt.total_length = 0;

                const auto& nets = networks->get_networks();
                auto it = nets.find(incoming_network);
                if (it != nets.end()) {
                    if (!it->second->send(src_mac, flip_ethertype_network(), &hereis_pkt, sizeof(hereis_pkt))) {
                        std::cerr << "Failed sending HEREIS response on network " << incoming_network << std::endl;
                    }
                }
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
                // Destination is local — deliver RPC replies to the associated client
                if (len >= sizeof(struct flip_packet) + sizeof(rpc_header) && fp->offset == 0) {
                    const rpc_header* rpc_hdr2 = (const rpc_header*)(packet + sizeof(struct flip_packet));
                    if (rpc_hdr2->type == AM_RPC_REPLY && on_local_rpc_reply) {
                        const uint8_t* payload = packet + sizeof(struct flip_packet) + sizeof(rpc_header);
                        size_t payload_len = len - sizeof(struct flip_packet) - sizeof(rpc_header);
                        on_local_rpc_reply(fp->dst_address, payload, payload_len);
                        send_rpc_ack(fp->dst_address, fp->src_address, rpc_hdr2);
                    }
                }
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

bool flip_router::install_local_address(flip_address_t address)
{
    if (address == 0) {
        return false;
    }

    auto existing = find_route(address);
    if (existing) {
        return existing->local;
    }

    auto route = std::make_shared<flip_route_entry>();
    route->dst_address = address;
    route->network = 0;
    route->next_hop_mac = hwaddr_t{0, 0, 0, 0, 0, 0};
    route->hopcount = 0;
    route->trusted = true;
    route->age = 0;
    route->local = true;
    routing_table[address] = route;
    return true;
}

void flip_router::remove_local_address(flip_address_t address)
{
    auto it = routing_table.find(address);
    if (it == routing_table.end()) {
        return;
    }

    if (it->second->local) {
        routing_table.erase(it);
        std::cout << "Removed local FLIP address " << address << std::endl;
    }
}

void flip_router::increment_age()
{
    // Increment the age of routing entries, remove stale entries, etc.
    // This will be called periodically to maintain the routing table
}

void flip_router::send_rpc_locate(flip_address_t src_addr, const rpc_port_t& port)
{
    struct flip_packet fp{};
    fp.version = 1;
    fp.type = static_cast<uint8_t>(flip_type::MULTIDATA);
    fp.flags = 0;
    fp.actual_hopcount = 0;
    fp.max_hopcount = 8;
    fp.dst_address = 0;
    fp.src_address = src_addr;
    fp.message_id = ++locate_tid;
    fp.length = sizeof(uint32_t) + sizeof(rpc_header);
    fp.offset = 0;
    fp.total_length = fp.length;

    uint32_t proto = htonl(PROTO_RPC);

    rpc_header rpc_hdr{};
    std::copy(port.begin(), port.end(), rpc_hdr.port);
    rpc_hdr.type = AM_RPC_LOCATE;
    rpc_hdr.flags = 0;
    rpc_hdr.tid = fp.message_id;
    rpc_hdr.dest = 0;
    rpc_hdr.from = 0;

    uint8_t buf[sizeof(flip_packet) + sizeof(uint32_t) + sizeof(rpc_header)];
    std::memcpy(buf,                              &fp,      sizeof(fp));
    std::memcpy(buf + sizeof(fp),                 &proto,   sizeof(proto));
    std::memcpy(buf + sizeof(fp) + sizeof(proto), &rpc_hdr, sizeof(rpc_hdr));

    // incoming_network = 0: no real network has this id, so all networks receive the LOCATE
    forward_broadcast(buf, sizeof(buf), 0);
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

void flip_router::send_rpc_ack(flip_address_t src, flip_address_t dst, const rpc_header* original_rpc_hdr)
{
    auto dst_route = find_route(dst);
    if (!dst_route) {
        std::cerr << "send_rpc_ack: no route to " << dst << std::endl;
        return;
    }

    struct flip_packet ack_fp{};
    ack_fp.version = 1;
    ack_fp.type = static_cast<uint8_t>(flip_type::UNIDATA);
    ack_fp.flags = 0;
    ack_fp.actual_hopcount = 0;
    ack_fp.max_hopcount = 8;
    ack_fp.dst_address = dst;
    ack_fp.src_address = src;
    ack_fp.message_id = ++locate_tid;
    ack_fp.length = sizeof(rpc_header);
    ack_fp.offset = 0;
    ack_fp.total_length = sizeof(rpc_header);

    rpc_header ack_rpc{};
    ack_rpc.kid = original_rpc_hdr->kid;
    std::copy(original_rpc_hdr->port, original_rpc_hdr->port + 6, ack_rpc.port);
    ack_rpc.type = AM_RPC_ACK;
    ack_rpc.flags = 0;
    ack_rpc.tid = original_rpc_hdr->tid;
    ack_rpc.dest = original_rpc_hdr->from;
    ack_rpc.from = original_rpc_hdr->dest;

    uint8_t buf[sizeof(flip_packet) + sizeof(rpc_header)];
    std::memcpy(buf, &ack_fp, sizeof(ack_fp));
    std::memcpy(buf + sizeof(ack_fp), &ack_rpc, sizeof(ack_rpc));

    forward_unicast(buf, sizeof(buf), dst_route->next_hop_mac, dst_route->network);
}

void flip_router::forward_broadcast(const uint8_t* packet, size_t len, flip_network_t incoming_network)
{
    if (!networks || len < sizeof(flip_packet)) return;

    uint8_t *fwd = new uint8_t[len + 1];
    if (fwd == nullptr) {
        std::cerr << "Packet allocation failed (" << len << " bytes)" << std::endl;
        return;
    }
    std::memcpy(fwd, packet, len);
    flip_packet* fwd_fp = reinterpret_cast<flip_packet*>(fwd);
    fwd_fp->actual_hopcount += 3;
    std::string pkt_type = packet_type_to_string((flip_type)fwd_fp->type);

    const hwaddr_t broadcast{0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const auto& nets = networks->get_networks();
    for (const auto& [net_id, driver] : nets) {
        if (net_id != incoming_network) {
            if (!fragment_and_send(driver, broadcast, FLIP_ETHERTYPE, fwd, len)) {
                std::cerr << "Failed to forward " << pkt_type << " to network " << net_id << std::endl;
            } else {
                std::cout << "Forwarded " << pkt_type << " to network " << net_id << std::endl;
            }
        }
    }
    delete[] fwd;
}

void flip_router::forward_unicast(const uint8_t* packet, size_t len, const hwaddr_t dst_mac, flip_network_t dst_network)
{
    if (!networks || len < sizeof(flip_packet)) return;

    uint8_t *fwd = new uint8_t[len + 1];
    if (fwd == nullptr) {
        std::cerr << "Packet allocation failed (" << len << " bytes)" << std::endl;
        return;
    }
    std::memcpy(fwd, packet, len);
    flip_packet* fwd_fp = reinterpret_cast<flip_packet*>(fwd);
    fwd_fp->actual_hopcount += 3;
    std::string pkt_type = packet_type_to_string((flip_type)fwd_fp->type);

    const auto& nets = networks->get_networks();
    auto it = nets.find(dst_network);
    if (it != nets.end()) {
        if (!fragment_and_send(it->second, dst_mac, FLIP_ETHERTYPE, fwd, len)) {
            std::cerr << "Failed to forward " << pkt_type << " to network " << dst_network << std::endl;
        } else {
            std::cout << "Forwarded " << pkt_type << " to network " << dst_network << std::endl;
        }
    }
    delete[] fwd;
}