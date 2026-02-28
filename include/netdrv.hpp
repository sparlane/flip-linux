#pragma once
#include <map>
#include <memory>
#include <cstdint>
#include <sys/types.h>
#include <array>

typedef std::array<uint8_t, 6> hwaddr_t;
typedef uint32_t flip_network_t;

class NetDrv
{
private:
    flip_network_t network_id{0};
public:
    virtual ~NetDrv() = default;
    virtual bool send(hwaddr_t dst, uint16_t proto, const void *buf, size_t len) = 0;
    virtual int get_fd() const = 0;
    virtual ssize_t recv(void* buf, size_t len) = 0;
    virtual hwaddr_t get_mac() const = 0;
    void set_network_id(flip_network_t id) { network_id = id; }
    flip_network_t get_network_id() const { return network_id; }
};

class flip_networks {
private:
    flip_network_t network_id{0};
    std::map<flip_network_t, std::shared_ptr<NetDrv>> networks;
public:
    flip_networks() = default;
    ~flip_networks() = default;
    void add_network(std::shared_ptr<NetDrv> driver) {
        driver->set_network_id(++this->network_id);
        networks[driver->get_network_id()] = driver;
    }
    const std::map<flip_network_t, std::shared_ptr<NetDrv>>& get_networks() const {
        return networks;
    }
};