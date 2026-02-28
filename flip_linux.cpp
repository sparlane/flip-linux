#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <cstring>
#include <csignal>
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
        std::cout << "Unix message from fd=" << client_fd << " type=" << type << " len=" << len << std::endl;
        (void)payload;
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