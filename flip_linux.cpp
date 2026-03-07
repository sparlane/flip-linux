#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <cstring>
#include <csignal>
#include <unordered_map>
#include <random>
#include <poll.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <linux/if_ether.h>

#include "tap.hpp"
#include "flip_router.hpp"
#include "unix_server.hpp"

std::unique_ptr<flip_router> router;
std::shared_ptr<flip_networks> networks;
std::unique_ptr<UnixServer> unix_server;
std::unordered_map<int, flip_address_t> unix_client_addresses;

struct ReassemblyKey {
    flip_address_t src_address;
    uint32_t message_id;
    bool operator==(const ReassemblyKey& o) const {
        return src_address == o.src_address && message_id == o.message_id;
    }
};

struct ReassemblyKeyHash {
    size_t operator()(const ReassemblyKey& k) const {
        return std::hash<uint64_t>()(k.src_address) ^ (std::hash<uint32_t>()(k.message_id) * 2654435761ULL);
    }
};

struct ReassemblyEntry {
    hwaddr_t src_mac;
    flip_network_t incoming_network;
    flip_packet header;
    std::vector<uint8_t> payload;
    uint32_t bytes_received;
};

static std::unordered_map<ReassemblyKey, ReassemblyEntry, ReassemblyKeyHash> reassembly_map;

static flip_address_t allocate_unix_client_address()
{
    static std::mt19937_64 rng(std::random_device{}());
    static constexpr flip_address_t kAddressMask = 0x00FFFFFFFFFFFFFFULL;

    for (int attempt = 0; attempt < 1024; ++attempt) {
        flip_address_t candidate = rng() & kAddressMask;
        if (candidate == 0) {
            continue;
        }
        if (router->install_local_address(candidate)) {
            return candidate;
        }
    }

    return 0;
}

static volatile sig_atomic_t should_exit = 0;

static void handle_sigint(int)
{
    should_exit = 1;
}

void recv_packet(const uint8_t* packet, size_t len, flip_network_t incoming_network)
{
    if (len < sizeof(struct ethhdr)) {
        std::cerr << "Received packet too short for Ethernet header" << std::endl;
        return;
    }

    struct ethhdr *eth = (struct ethhdr *)packet;
    if (eth->h_proto != flip_ethertype_network()) {
        // Not a FLIP packet, ignore
        return;
    }

    if (len < sizeof(struct ethhdr) + sizeof(struct fc_header)) {
        std::cerr << "Received packet too short for Fragment header" << std::endl;
        return;
    }

    struct fc_header *fc = (struct fc_header *)(packet + sizeof(struct ethhdr));
    if (fc->fc_type == 0)
    {
        const uint8_t* flip_data = packet + sizeof(struct ethhdr) + sizeof(struct fc_header);
        size_t flip_len = len - sizeof(struct ethhdr) - sizeof(struct fc_header);

        if (flip_len < sizeof(struct flip_packet)) {
            std::cerr << "Fragment too short for FLIP header" << std::endl;
            return;
        }

        const struct flip_packet* fp = (const struct flip_packet*)flip_data;
        uint32_t frag_offset = fp->offset;
        uint32_t frag_length = fp->length;
        uint32_t total_length = fp->total_length;

        // Not fragmented: deliver directly
        if (frag_offset == 0 && (total_length == 0 || frag_length == total_length)) {
            router->route_packet(std::to_array(eth->h_source), flip_data, flip_len, incoming_network);
            return;
        }

        // Fragmented: reassemble
        ReassemblyKey key{fp->src_address, fp->message_id};

        if (frag_offset == 0) {
            // First fragment: initialise entry
            ReassemblyEntry& entry = reassembly_map[key];
            entry.src_mac = std::to_array(eth->h_source);
            entry.incoming_network = incoming_network;
            entry.header = *fp;
            entry.payload.assign(total_length, 0);
            entry.bytes_received = 0;
        }

        auto it = reassembly_map.find(key);
        if (it == reassembly_map.end()) {
            std::cerr << "Out-of-order fragment (no first fragment yet), discarding" << std::endl;
            return;
        }

        ReassemblyEntry& entry = it->second;
        const uint8_t* frag_payload = flip_data + sizeof(struct flip_packet);
        size_t frag_payload_len = flip_len - sizeof(struct flip_packet);

        if (frag_offset + frag_length > total_length || frag_length > frag_payload_len) {
            std::cerr << "Invalid fragment bounds, discarding reassembly" << std::endl;
            reassembly_map.erase(it);
            return;
        }

        std::memcpy(entry.payload.data() + frag_offset, frag_payload, frag_length);
        entry.bytes_received += frag_length;

        if (entry.bytes_received >= total_length) {
            // Reassembly complete: reconstruct full packet and deliver
            entry.header.offset = 0;
            entry.header.length = total_length;
            entry.header.total_length = total_length;

            std::vector<uint8_t> full_packet(sizeof(flip_packet) + total_length);
            std::memcpy(full_packet.data(), &entry.header, sizeof(flip_packet));
            std::memcpy(full_packet.data() + sizeof(flip_packet), entry.payload.data(), total_length);

            hwaddr_t src_mac = entry.src_mac;
            flip_network_t net = entry.incoming_network;
            reassembly_map.erase(it);
            router->route_packet(src_mac, full_packet.data(), full_packet.size(), net);
        }
    }
    else if (fc->fc_type == 1)
    {
        constexpr size_t MIN_PAYLOAD = 60 - sizeof(struct ethhdr);
        uint8_t response[MIN_PAYLOAD] = {};
        struct fc_header *resp_fc = reinterpret_cast<struct fc_header *>(response);
        resp_fc->fc_type = 2;
        resp_fc->fc_cnt = 5;
        auto net = networks->get_networks().at(incoming_network);
        net->send(std::to_array(eth->h_source), FLIP_ETHERTYPE, response, sizeof(response));
    }
}

uint64_t kid_alloc = 1;

int main(int argc, char* argv[])
{
    std::signal(SIGINT, handle_sigint);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " tapX [tapY ...]" << std::endl;
        return 1;
    }

    networks = std::make_shared<flip_networks>();
    std::vector<std::shared_ptr<Tap>> tap_devs;
    std::vector<struct pollfd> pfds;

    // Add each tap device from argv
    for (int i = 1; i < argc; ++i) {
        std::shared_ptr<Tap> tap = std::make_shared<Tap>(argv[i]);
        networks->add_network(tap);
        tap_devs.push_back(tap);
        struct pollfd pfd = {};
        pfd.fd = tap->get_fd();
        pfd.events = POLLIN;
        pfds.push_back(pfd);
    }

    // Add timerfd for 30s timer
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        std::cerr << "Failed to create timerfd: " << strerror(errno) << std::endl;
        return 1;
    }
    struct itimerspec timer_spec = {};
    timer_spec.it_interval.tv_sec = 30;
    timer_spec.it_value.tv_sec = 30;
    timerfd_settime(timer_fd, 0, &timer_spec, nullptr);
    struct pollfd timer_pfd = {};
    timer_pfd.fd = timer_fd;
    timer_pfd.events = POLLIN;
    pfds.push_back(timer_pfd);

    router = std::make_unique<flip_router>(networks);
    router->set_local_rpc_reply_cb([](flip_address_t dst, const uint8_t* payload, size_t len) {
        for (const auto& [fd, addr] : unix_client_addresses) {
            if (addr == dst) {
                unix_server->send_to_client(fd, UNIX_MSG_TRANS, payload, len);
                return;
            }
        }
        std::cerr << "No unix client for local FLIP address " << dst << std::endl;
    });

    // Start Unix socket server
    unix_server = std::make_unique<UnixServer>("/tmp/flip.sock");
    if (!unix_server->start()) {
        std::cerr << "Failed to start Unix server" << std::endl;
        return 1;
    }
    unix_server->set_on_message([](int client_fd, uint32_t type, const uint8_t* payload, size_t len) {
        if (type == UNIX_MSG_TRANS) {
            if (len < sizeof(am_header)) {
                std::cerr << "Unix trans message too short from fd=" << client_fd << std::endl;
                return;
            }
            auto addr_it = unix_client_addresses.find(client_fd);
            if (addr_it == unix_client_addresses.end()) {
                std::cerr << "No FLIP address for unix client fd=" << client_fd << std::endl;
                return;
            }

            auto hdr = std::make_shared<am_header>();
            std::memcpy(hdr.get(), payload, sizeof(am_header));
            flip_address_t src_addr = addr_it->second;

            rpc_port_t port_array;
            std::copy(hdr->port, hdr->port + 6, port_array.begin());

            // Capture am_header + data for later use (shared to allow copy into std::function)
            auto trans_data = std::make_shared<std::vector<uint8_t>>(payload + sizeof(am_header), payload + len);

            static uint32_t trans_tid{0};
            auto send_unidata = [src_addr, hdr, trans_data](flip_address_t dst_addr) {
                struct flip_packet fp{};
                fp.version = 1;
                fp.type = static_cast<uint8_t>(flip_type::UNIDATA);
                fp.actual_hopcount = 0;
                fp.max_hopcount = 8;
                fp.dst_address = dst_addr;
                fp.src_address = src_addr;
                fp.message_id = ++trans_tid;
                fp.length = static_cast<uint32_t>(sizeof(rpc_header) + sizeof(am_header) + trans_data->size());
                fp.offset = 0;
                fp.total_length = fp.length;

                struct rpc_header rpc_hdr{};
                rpc_hdr.kid = kid_alloc++;
                std::copy(hdr->port, hdr->port + 6, rpc_hdr.port);
                rpc_hdr.type = AM_RPC_REQUEST;
                rpc_hdr.flags = 0;
                rpc_hdr.tid = ntohs(fp.message_id);
                rpc_hdr.dest = 0; // Not used for UNIDATA
                rpc_hdr.from = 0; // Not used for UNIDATA

                std::vector<uint8_t> pkt_buf(sizeof(flip_packet) + fp.length);
                std::memcpy(pkt_buf.data(), &fp, sizeof(flip_packet));
                std::memcpy(pkt_buf.data() + sizeof(flip_packet), &rpc_hdr, sizeof(rpc_header));
                std::memcpy(pkt_buf.data() + sizeof(flip_packet) + sizeof(rpc_header), hdr.get(), sizeof(am_header));
                if (!trans_data->empty()) {
                    std::memcpy(pkt_buf.data() + sizeof(flip_packet) + sizeof(rpc_header) + sizeof(am_header), trans_data->data(), trans_data->size());
                }

                static const hwaddr_t local_mac{};
                router->route_packet(local_mac, pkt_buf.data(), pkt_buf.size(), 0);
            };

            // Check if destination port is local
            auto rpc_mgr = router->get_rpc_port_manager();
            auto local_binding = rpc_mgr->get_local_binding(port_array);
            if (local_binding.has_value()) {
                auto dst_it = unix_client_addresses.find(local_binding->client_fd);
                if (dst_it != unix_client_addresses.end()) {
                    send_unidata(dst_it->second);
                    return;
                }
            }

            // Remote: only send LOCATE if no outstanding lookup for this port is already in flight
            bool need_locate = !rpc_mgr->has_pending_lookup(port_array);
            rpc_mgr->begin_remote_lookup(port_array, client_fd,
                [send_unidata](int, const rpc_port_t&, const std::string& remote_socket, bool found) {
                    if (!found) return;
                    send_unidata(std::stoull(remote_socket));
                });
            if (need_locate) {
                router->send_rpc_locate(src_addr, port_array);
            }
        }
    });
    unix_server->set_on_connect([](int client_fd) {
        flip_address_t addr = allocate_unix_client_address();
        if (addr == 0) {
            std::cerr << "Failed to allocate FLIP address for unix client fd=" << client_fd << std::endl;
            ::close(client_fd);
            return;
        }

        unix_client_addresses[client_fd] = addr;
        std::cout << "Assigned FLIP address " << addr << " to unix client fd=" << client_fd << std::endl;
    });
    unix_server->set_on_disconnect([](int client_fd) {
        auto it = unix_client_addresses.find(client_fd);
        if (it != unix_client_addresses.end()) {
            router->remove_local_address(it->second);
            unix_client_addresses.erase(it);
        }
    });

    constexpr size_t BUF_SIZE = 2048;
    uint8_t buf[BUF_SIZE];

    while (!should_exit) {
        // Rebuild poll set each iteration to account for new/removed unix clients
        pfds.resize(tap_devs.size() + 1); // tap fds + timer fd

        // Add unix listen fd
        struct pollfd unix_listen_pfd = {};
        unix_listen_pfd.fd = unix_server->get_listen_fd();
        unix_listen_pfd.events = POLLIN;
        pfds.push_back(unix_listen_pfd);

        // Add unix client fds
        auto unix_client_fds = unix_server->get_client_fds();
        size_t unix_clients_start = pfds.size();
        for (int cfd : unix_client_fds) {
            struct pollfd cpfd = {};
            cpfd.fd = cfd;
            cpfd.events = POLLIN;
            pfds.push_back(cpfd);
        }

        int ret = poll(pfds.data(), pfds.size(), -1);
        if (ret < 0) {
            if (errno == EINTR) {
                if (should_exit) {
                    break;
                }
                continue;
            }
            std::cerr << "poll error: " << strerror(errno) << std::endl;
            break;
        }

        // Handle tap device events
        for (size_t i = 0; i < tap_devs.size(); ++i) {
            if (pfds[i].revents & POLLIN) {
                ssize_t n = tap_devs[i]->recv(buf, BUF_SIZE);
                if (n > 0) {
                    std::cout << "Received packet of size " << n << " from tap " << i << std::endl;
                    recv_packet(buf, n, tap_devs[i]->get_network_id());
                } else {
                    std::cerr << "Error reading from tap " << argv[i+1] << ": " << strerror(errno) << std::endl;
                }
            }
        }

        // Timer event (index = tap_devs.size())
        if (pfds[tap_devs.size()].revents & POLLIN) {
            uint64_t expirations;
            read(timer_fd, &expirations, sizeof(expirations));
            std::cout << "Timer event: 30 seconds elapsed" << std::endl;
            router->increment_age();
        }

        // Unix listen socket (index = tap_devs.size() + 1)
        if (pfds[tap_devs.size() + 1].revents & POLLIN) {
            unix_server->accept_client();
        }

        // Unix client sockets
        for (size_t i = 0; i < unix_client_fds.size(); ++i) {
            if (pfds[unix_clients_start + i].revents & POLLIN) {
                unix_server->handle_client_data(unix_client_fds[i]);
            }
        }
    }

    unix_server->stop();
    close(timer_fd);
    return 0;
}