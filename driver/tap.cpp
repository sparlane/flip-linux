#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>

#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>

#include "tap.hpp"

Tap::Tap(const char *dev)
{
    struct ifreq ifr = {};

    if ((this->fd = open ("/dev/net/tun", O_RDWR)) < 0) {
        throw std::runtime_error("Failed to open /dev/net/tun");
    }

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    strncpy (ifr.ifr_name, dev, IFNAMSIZ);

    if (ioctl(this->fd, TUNSETIFF, (void *) &ifr) < 0) {
        close(this->fd);
        this->fd = -1;
        throw std::runtime_error("Failed to set TUN interface");
    }

    std::cout << "Allocated TAP interface: " << ifr.ifr_name << std::endl;

    ioctl(this->fd, SIOCGIFHWADDR, &ifr);
    memcpy(this->mac.data(), ifr.ifr_hwaddr.sa_data, 6);
    std::cout << "TAP MAC Address: " << std::hex << (int)this->mac[0] << ":" << (int)this->mac[1] << ":" << (int)this->mac[2] << ":" << (int)this->mac[3] << ":" << (int)this->mac[4] << ":" << (int)this->mac[5] << std::endl;
}

Tap::~Tap()
{
    std::cout << "Closing TAP interface" << std::endl;
    if (this->fd >= 0) {
        close(this->fd);
    }
}

hwaddr_t Tap::get_mac() const
{
    return this->mac;
}

bool Tap::send(void *buf, size_t len)
{
    return write(this->fd, buf, len) == (ssize_t)len;
}

int Tap::get_fd() const
{
    return this->fd;
}

ssize_t Tap::recv(void *buf, size_t len)
{
    return read(this->fd, buf, len);
}