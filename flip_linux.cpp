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
        router->route_packet(std::to_array(eth->h_source), packet + sizeof(struct ethhdr) + sizeof(struct fc_header), len - sizeof(struct ethhdr) - sizeof(struct fc_header), incoming_network);
    }
}

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
                fp.length = static_cast<uint32_t>(sizeof(rpc_header) + sizeof(am_header) + hdr->bufsize);
                fp.offset = 0;
                fp.total_length = fp.length;

                struct rpc_header rpc_hdr{};
                rpc_hdr.kid = 0; // Not used for UNIDATA
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
                std::memcpy(pkt_buf.data() + sizeof(flip_packet) + sizeof(rpc_header) + sizeof(am_header), trans_data->data(), hdr->bufsize);

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
        } else {
            std::cout << "Unix message from fd=" << client_fd << " type=" << type << " len=" << len << std::endl;
            (void)payload;
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