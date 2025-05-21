#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

void printSmaps() {
    std::ifstream smaps("/proc/self/smaps");

    if (!smaps.is_open()) {
        std::cerr << "Could not open /proc/self/smaps" << std::endl;
        return;
    }

    std::string line;
    std::ofstream log("log.txt", std::ios::app);
    while (std::getline(smaps, line)) {
        // if (line.find("Size:") == 0 ||
        //     line.find("Rss:") == 0 ||
        //     line.find("Shared_Clean:") == 0 ||
        //     line.find("Private_Clean:") == 0 ||
        //     line.find("KernelPageSize:") == 0) {
            log << line;
        // }
    }
}

int main() {
    printSmaps();
    return 0;
}

