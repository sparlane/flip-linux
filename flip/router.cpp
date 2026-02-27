#include <iostream>
#include "flip_router.hpp"

flip_router::flip_router()
{
    // Initialize routing table, etc.
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

    std::cout << "Received " << fp->type << " packet from " << (int)src_mac[0] << ":" << (int)src_mac[1] << ":" << (int)src_mac[2] << ":" << (int)src_mac[3] << ":" << (int)src_mac[4] << ":" << (int)src_mac[5] << std::endl;

    switch ((flip_type)fp->type)
    {
        case flip_type::LOCATE:
            if (fp->actual_hopcount == fp->max_hopcount && dst_route && dst_route->local) {
                std::cout << "Destination " << fp->dst_address << " is local, sending HEREIS response" << std::endl;
                // Send HEREIS response back to src_mac
            } else {
                // Forward LOCATE packet to next hop(s) based on routing table
            }
            break;
        case flip_type::HEREIS:
            // Route already added by the source based route adding.
            // May need to forward this packet
            break;
        case flip_type::UNIDATA:
            // Need to queue/send this packet to the local application if dst is local, or forward to next hop if not
            break;
        case flip_type::MULTIDATA:
            break;
        case flip_type::NOTHERE:
            if (dst_route && dst_route->network == incoming_network) {
                std::cout << "Received NOTHERE for destination " << fp->dst_address << " on network " << incoming_network << ", removing route" << std::endl;
                routing_table.erase(fp->dst_address);
            }
            // May need to forward NOTHERE to the source of the original packet if this is a response to a LOCATE
            break;
        case flip_type::UNTRUSTED:
            // Handle UNTRUSTED packet
            // May need to forward UNTRUSTED to the source of the original packet if this is a response to a LOCATE
            break;
        default:
            std::cerr << "Received packet with unknown FLIP type: " << (int)fp->type << std::endl;
            break;
    }
    // Process the incoming packet, determine routing, and forward as needed
    // This is where the main routing logic will be implemented
}

void flip_router::increment_age()
{
    // Increment the age of routing entries, remove stale entries, etc.
    // This will be called periodically to maintain the routing table
}