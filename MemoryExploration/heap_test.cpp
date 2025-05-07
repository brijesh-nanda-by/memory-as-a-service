#include <iostream>
using namespace std;

const size_t HEAP_LIMIT = 16384;
size_t heap_usage = 0;

void allocateMemory(size_t size) {

    if (heap_usage+size > HEAP_LIMIT) {
        cerr << "Limit exceed .." << endl;
    }

    char* ptr = new (nothrow) char[size];
    if (ptr == nullptr) {
        cerr << "Allocation failed" << endl;
        return;
    }

    heap_usage += size;
    cout << "Allocated " << size << " bytes. Total heap usage: " << heap_usage << " bytes." << endl;

    
} 

void deletePtr(char* ptr) {
    free(ptr);
    heap_usage -= sizeof(*ptr);
    cout << sizeof(*ptr) << " freed " << endl;
}

int main() {
    // Simulate allocations
    allocateMemory(4000); // OK
    allocateMemory(8000); // OK
    allocateMemory(6000); // This will exceed 16 KB and trigger a warning

    return 0;
}
 