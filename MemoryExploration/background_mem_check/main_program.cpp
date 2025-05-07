#include <thread>
#include "memory_monitor.h"
#include <iostream>
using namespace std;

int main(int argc, char **argv)
{
    std::cout << "Inside main program" << endl;
    std::thread monitor(monitorMemory);
    monitor.detach();

    // Simulate allocations
    char *a = new char[10000];
    char *b = new char[8000];

    for (int i = 0; i < 10000; i += 4096)
        a[i] = 'x';
    for (int i = 0; i < 8000; i += 4096)
        b[i] = 'x';

    std::this_thread::sleep_for(std::chrono::seconds(20));
    delete[] a;
    delete[] b;
    return 0;
}