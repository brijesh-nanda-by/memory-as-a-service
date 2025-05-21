#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <iomanip>

bool isAddressMapped(void* addr) {
    uintptr_t address = reinterpret_cast<uintptr_t>(addr);
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line)) {
        std::istringstream iss(line);
        std::string addr_range;
        iss >> addr_range;
        std::cout << addr_range << std::endl;
        size_t dash_pos = addr_range.find('-');
        uintptr_t start = std::stoul(addr_range.substr(0, dash_pos), nullptr, 16);
        uintptr_t end   = std::stoul(addr_range.substr(dash_pos + 1), nullptr, 16);

        if (address >= start && address < end) {
            return true;
        }
    }
    return false;
}

int main() {
    int* ptr = new int;
    std::cout << "Is ptr mapped? " << std::boolalpha << isAddressMapped(ptr) << std::endl;

    int fake = 123;
    std::cout << "Is &fake mapped? " << isAddressMapped(&fake) << std::endl;

    delete ptr;

    // Example with random address
    void* bad = reinterpret_cast<void*>(0xdeadbeef);
    std::cout << "Is 0xdeadbeef mapped? " << isAddressMapped(bad) << std::endl;

    return 0;
}