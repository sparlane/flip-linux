#include <iostream>
#include <memory>

#include "tap.hpp"
#include "flip_router.hpp"

std::unique_ptr<flip_router> router;
std::unique_ptr<flip_networks> networks;

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " tapX" << std::endl;
        return 1;
    }

    std::shared_ptr<Tap> netdrv = std::make_shared<Tap>(argv[1]);
    networks = std::make_unique<flip_networks>();
    networks->add_network(netdrv);

    router = std::make_unique<flip_router>();

    return 0;
}