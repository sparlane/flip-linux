#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <linux/if_ether.h>

#include "tap.hpp"
#include "flip_router.hpp"

std::unique_ptr<flip_router> router;
std::unique_ptr<flip_networks> networks;

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
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " tapX [tapY ...]" << std::endl;
        return 1;
    }

    networks = std::make_unique<flip_networks>();
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

    router = std::make_unique<flip_router>();

    constexpr size_t BUF_SIZE = 2048;
    uint8_t buf[BUF_SIZE];

    while (true) {
        int ret = poll(pfds.data(), pfds.size(), -1);
        if (ret < 0) {
            std::cerr << "poll error: " << strerror(errno) << std::endl;
            break;
        }
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
        // Timer event is last in pfds
        if (pfds.back().revents & POLLIN) {
            uint64_t expirations;
            read(timer_fd, &expirations, sizeof(expirations));
            std::cout << "Timer event: 30 seconds elapsed" << std::endl;
            router->increment_age();
        }
    }

    close(timer_fd);
    return 0;
}