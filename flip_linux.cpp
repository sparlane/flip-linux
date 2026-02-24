#include <iostream>
#include <memory>

#include "tap.hpp"

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " tapX" << std::endl;
        return 1;
    }

    std::shared_ptr<Tap> netdrv = std::make_shared<Tap>(argv[1]);

    return 0;
}