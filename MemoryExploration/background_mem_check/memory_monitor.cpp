#include <cstddef>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <sstream>
using namespace std;
const size_t HEAP_SIZE = 16;

size_t getMemoryKB()
{
    // std::ofstream log("log.txt", std::ios::app);

    ifstream status("/proc/self/status");
    string line;
    stringstream buffer;
    // buffer << status.rdbuf();
    // log << "buffer : ";
    // log << buffer.str();
    while (getline(status, line))
    {
        if (line.find("VmRSS:") == 0)
        {
            size_t kb;
            sscanf(line.c_str(), "VmRSS: %zu kb", &kb);
            // cout << "Memory used : " << kb << " KB" << endl;
            return kb;
        }
    }
    return 0;
}

void monitorMemory()
{
    std::ofstream log("log.txt", std::ios::app);

    if (!log.is_open())
    {
        std::cerr << " Failed to open log file " << std::endl;
        ;
    }

    while (true)
    {

        // monitor every second
        for (int i = 0; i < 20; ++i)
        {
            // print memory
            size_t usage_kb = getMemoryKB();
            log << "[Memory Monitor] Usage: " << usage_kb << " KB" << std::endl;
            if (usage_kb > HEAP_SIZE)
            {
                log << "Process exceeded its memory usuage " << endl;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
