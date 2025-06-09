#include <iostream>
#include <unordered_map>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <functional>
#include <exception>
#include <shared_mutex>

// Forward declarations
class StorageNode;
class ControllerNode;

// Global pointer structure
struct GlobalPointer {
    uint64_t address;
    size_t size;
    std::string data_type;
    
    GlobalPointer(uint64_t addr, size_t sz, const std::string& type) 
        : address(addr), size(sz), data_type(type) {}
    
    bool operator==(const GlobalPointer& other) const {
        return address == other.address && size == other.size && data_type == other.data_type;
    }
};

// Hash function for GlobalPointer
struct GlobalPointerHash {
    std::size_t operator()(const GlobalPointer& gp) const {
        std::size_t h1 = std::hash<uint64_t>{}(gp.address);
        std::size_t h2 = std::hash<size_t>{}(gp.size);
        std::size_t h3 = std::hash<std::string>{}(gp.data_type);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

// Node status enum
enum class NodeStatus {
    ACTIVE,
    MIGRATING,
    FAILED
};

// Node information
struct NodeInfo {
    std::string node_id;
    std::string host;
    int port;
    size_t memory_capacity;
    std::atomic<size_t> memory_used{0};
    std::atomic<NodeStatus> status{NodeStatus::ACTIVE};
    bool is_spot_instance;
    
    NodeInfo(const std::string& id, const std::string& h, int p, size_t capacity, bool spot = true)
        : node_id(id), host(h), port(p), memory_capacity(capacity), is_spot_instance(spot) {}
};

// Exception for out-of-bounds access
class OutOfBoundsException : public std::exception {
private:
    std::string message;
public:
    OutOfBoundsException(const std::string& msg) : message(msg) {}
    const char* what() const noexcept override { return message.c_str(); }
};

// Local translator for address translation
class LocalTranslator {
private:
    std::string node_id;
    std::unordered_map<uint64_t, std::pair<uint64_t, size_t>> local_mappings;
    mutable std::shared_mutex mutex;

public:
    LocalTranslator(const std::string& id) : node_id(id) {}
    
    // Translate global address to local address
    std::pair<uint64_t, size_t> translate(uint64_t global_addr, size_t size) {
        std::shared_lock<std::shared_mutex> lock(mutex);
        
        auto it = local_mappings.find(global_addr);
        if (it != local_mappings.end()) {
            auto [local_addr, local_size] = it->second;
            if (local_size >= size) {
                return {local_addr, std::min(size, local_size)};
            }
        }
        
        // Throw out-of-bounds exception if data not found locally
        throw OutOfBoundsException("Data not found on node " + node_id + 
                                 " for global address " + std::to_string(global_addr));
    }
    
    void add_mapping(uint64_t global_addr, uint64_t local_addr, size_t size) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        local_mappings[global_addr] = {local_addr, size};
    }
    
    void remove_mapping(uint64_t global_addr) {
        std::unique_lock<std::shared_mutex> lock(mutex);
        local_mappings.erase(global_addr);
    }
    
    size_t get_mapping_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return local_mappings.size();
    }
};

// Storage node class
class StorageNode {
private:
    std::unique_ptr<NodeInfo> info;
    std::unordered_map<uint64_t, std::vector<uint8_t>> memory;
    std::unique_ptr<LocalTranslator> translator;
    mutable std::shared_mutex memory_mutex;
    std::atomic<uint64_t> next_local_addr{1000};
    ControllerNode* controller;

public:
    StorageNode(std::unique_ptr<NodeInfo> node_info, ControllerNode* ctrl) 
        : info(std::move(node_info)), controller(ctrl) {
        translator = std::make_unique<LocalTranslator>(info->node_id);
    }
    
    // Allocate local memory and return local address
    uint64_t allocate_local_memory(size_t size, const std::vector<uint8_t>& data) {
        std::unique_lock<std::shared_mutex> lock(memory_mutex);
        
        if (info->memory_used.load() + size > info->memory_capacity) {
            throw std::runtime_error("Not enough memory on node " + info->node_id);
        }
        
        uint64_t local_addr = next_local_addr.fetch_add(size);
        memory[local_addr] = data;
        info->memory_used.fetch_add(size);
        
        return local_addr;
    }
    
    // Read data from local address
    std::vector<uint8_t> read_local(uint64_t local_addr) const {
        std::shared_lock<std::shared_mutex> lock(memory_mutex);
        
        auto it = memory.find(local_addr);
        if (it != memory.end()) {
            return it->second;
        }
        return {};
    }
    
    // Write data to local address
    void write_local(uint64_t local_addr, const std::vector<uint8_t>& data) {
        std::unique_lock<std::shared_mutex> lock(memory_mutex);
        memory[local_addr] = data;
    }
    
    // DFlow implementation - bring data to this node
    std::vector<uint8_t> dflow(uint64_t global_addr, size_t size);
    
    // Get node information
    const NodeInfo& get_info() const { return *info; }
    
    // Get translator
    LocalTranslator* get_translator() { return translator.get(); }
    
    // Execute stored procedure with potential cross-node access
    template<typename Func>
    auto execute_sproc(uint64_t start_addr, size_t size, Func&& sproc) -> decltype(sproc(std::vector<uint8_t>{})) {
        try {
            // Try to access data locally first
            auto [local_addr, local_size] = translator->translate(start_addr, size);
            auto data = read_local(local_addr);
            return sproc(data);
        } catch (const OutOfBoundsException& e) {
            // Data not local, use DFlow to bring it here
            std::cout << "Out-of-bounds access detected: " << e.what() << std::endl;
            std::cout << "Executing DFlow to bring data to node " << info->node_id << std::endl;
            
            auto data = dflow(start_addr, size);
            return sproc(data);
        }
    }
    
    // Simulate spot instance reclamation
    void simulate_reclamation() {
        info->status.store(NodeStatus::MIGRATING);
        std::cout << "ALERT: Spot instance " << info->node_id << " is being reclaimed!" << std::endl;
        
        // In a real implementation, this would trigger migration to other nodes
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        info->status.store(NodeStatus::FAILED);
        std::cout << "Node " << info->node_id << " has been reclaimed." << std::endl;
    }
};

// Controller node managing global mappings
class ControllerNode {
private:
    std::unordered_map<std::string, std::unique_ptr<StorageNode>> nodes;
    std::unordered_map<uint64_t, std::string> global_mappings; // global_addr -> node_id
    std::unordered_map<GlobalPointer, std::string, GlobalPointerHash> pointer_registry;
    mutable std::shared_mutex controller_mutex;
    std::atomic<uint64_t> next_global_addr{10000};

public:
    ControllerNode() = default;
    
    // Register a storage node
    void register_node(std::unique_ptr<StorageNode> node) {
        std::unique_lock<std::shared_mutex> lock(controller_mutex);
        std::string node_id = node->get_info().node_id;
        nodes[node_id] = std::move(node);
        std::cout << "Registered node: " << node_id << std::endl;
    }
    
    // Allocate memory across nodes and return global pointer
    GlobalPointer allocate_memory(size_t size, const std::vector<uint8_t>& data, const std::string& data_type) {
        std::unique_lock<std::shared_mutex> lock(controller_mutex);
        
        // Find a suitable node (simple round-robin for now)
        StorageNode* selected_node = nullptr;
        for (auto& [node_id, node] : nodes) {
            if (node->get_info().status.load() == NodeStatus::ACTIVE &&
                node->get_info().memory_used.load() + size <= node->get_info().memory_capacity) {
                selected_node = node.get();
                break;
            }
        }
        
        if (!selected_node) {
            throw std::runtime_error("No suitable node found for allocation");
        }
        
        // Allocate on selected node
        uint64_t local_addr = selected_node->allocate_local_memory(size, data);
        
        // Create global pointer
        uint64_t global_addr = next_global_addr.fetch_add(size);
        GlobalPointer gp(global_addr, size, data_type);
        
        // Update mappings
        global_mappings[global_addr] = selected_node->get_info().node_id;
        pointer_registry[gp] = selected_node->get_info().node_id;
        selected_node->get_translator()->add_mapping(global_addr, local_addr, size);
        
        std::cout << "Allocated " << size << " bytes on node " << selected_node->get_info().node_id 
                  << " with global address " << global_addr << std::endl;
        
        return gp;
    }
    
    // Resolve global address to node
    StorageNode* resolve_node(uint64_t global_addr) {
        std::shared_lock<std::shared_mutex> lock(controller_mutex);
        
        auto it = global_mappings.find(global_addr);
        if (it != global_mappings.end()) {
            auto node_it = nodes.find(it->second);
            if (node_it != nodes.end()) {
                return node_it->second.get();
            }
        }
        return nullptr;
    }
    
    // Get all nodes
    std::vector<StorageNode*> get_all_nodes() {
        std::shared_lock<std::shared_mutex> lock(controller_mutex);
        std::vector<StorageNode*> result;
        for (auto& [node_id, node] : nodes) {
            result.push_back(node.get());
        }
        return result;
    }
    
    // Print system status
    void print_status() const {
        std::shared_lock<std::shared_mutex> lock(controller_mutex);
        
        std::cout << "\n=== CompuCache System Status ===" << std::endl;
        std::cout << "Total nodes: " << nodes.size() << std::endl;
        std::cout << "Global mappings: " << global_mappings.size() << std::endl;
        
        for (const auto& [node_id, node] : nodes) {
            const auto& info = node->get_info();
            std::cout << "Node " << node_id << ": " 
                      << info.memory_used.load() << "/" << info.memory_capacity 
                      << " bytes used, Status: ";
            
            switch (info.status.load()) {
                case NodeStatus::ACTIVE: std::cout << "ACTIVE"; break;
                case NodeStatus::MIGRATING: std::cout << "MIGRATING"; break;
                case NodeStatus::FAILED: std::cout << "FAILED"; break;
            }
            
            std::cout << ", Mappings: " << node->get_translator()->get_mapping_count() << std::endl;
        }
        std::cout << "==============================\n" << std::endl;
    }
};

// Implementation of DFlow in StorageNode
std::vector<uint8_t> StorageNode::dflow(uint64_t global_addr, size_t size) {
    if (!controller) {
        throw std::runtime_error("No controller available for DFlow");
    }
    
    // Find the node that has this data
    StorageNode* source_node = controller->resolve_node(global_addr);
    if (!source_node) {
        throw std::runtime_error("No node found for global address " + std::to_string(global_addr));
    }
    
    if (source_node == this) {
        // Data is already on this node, just read it locally
        auto [local_addr, local_size] = translator->translate(global_addr, size);
        return read_local(local_addr);
    }
    
    // Fetch data from remote node
    std::cout << "DFlow: Fetching data from node " << source_node->get_info().node_id 
              << " to node " << info->node_id << std::endl;
    
    try {
        auto [remote_local_addr, remote_size] = source_node->get_translator()->translate(global_addr, size);
        auto data = source_node->read_local(remote_local_addr);
        
        // Cache the data locally for future access
        uint64_t local_addr = allocate_local_memory(data.size(), data);
        translator->add_mapping(global_addr, local_addr, data.size());
        
        return data;
    } catch (const OutOfBoundsException& e) {
        throw std::runtime_error("DFlow failed: " + std::string(e.what()));
    }
}

// Example stored procedures
namespace StoredProcedures {
    // Simple aggregation sproc
    uint64_t sum_array(const std::vector<uint8_t>& data) {
        uint64_t sum = 0;
        for (size_t i = 0; i + sizeof(uint64_t) <= data.size(); i += sizeof(uint64_t)) {
            uint64_t value;
            std::memcpy(&value, &data[i], sizeof(uint64_t));
            sum += value;
        }
        return sum;
    }
    
    // Pointer chasing sproc
    std::vector<uint64_t> chase_pointers(const std::vector<uint8_t>& data) {
        std::vector<uint64_t> pointers;
        for (size_t i = 0; i + sizeof(uint64_t) <= data.size(); i += sizeof(uint64_t)) {
            uint64_t pointer;
            std::memcpy(&pointer, &data[i], sizeof(uint64_t));
            pointers.push_back(pointer);
        }
        return pointers;
    }
}

// Demo function
void run_compucache_demo() {
    std::cout << "=== CompuCache DFlow System Demo ===" << std::endl;
    
    // Create controller
    auto controller = std::make_unique<ControllerNode>();
    
    // Create storage nodes
    auto node1 = std::make_unique<StorageNode>(
        std::make_unique<NodeInfo>("node-1", "192.168.1.1", 8080, 1024*1024, true),
        controller.get()
    );
    
    auto node2 = std::make_unique<StorageNode>(
        std::make_unique<NodeInfo>("node-2", "192.168.1.2", 8081, 1024*1024, true),
        controller.get()
    );
    
    auto node3 = std::make_unique<StorageNode>(
        std::make_unique<NodeInfo>("node-3", "192.168.1.3", 8082, 1024*1024, false),
        controller.get()
    );
    
    // Register nodes
    controller->register_node(std::move(node1));
    controller->register_node(std::move(node2));
    controller->register_node(std::move(node3));
    
    controller->print_status();
    
    // Allocate some data
    std::vector<uint8_t> data1(32);
    std::vector<uint8_t> data2(64);
    std::vector<uint8_t> data3(128);
    
    // Fill with sample data
    std::iota(data1.begin(), data1.end(), 1);
    std::iota(data2.begin(), data2.end(), 100);
    std::iota(data3.begin(), data3.end(), 200);
    
    auto gp1 = controller->allocate_memory(data1.size(), data1, "array");
    auto gp2 = controller->allocate_memory(data2.size(), data2, "array");
    auto gp3 = controller->allocate_memory(data3.size(), data3, "array");
    
    controller->print_status();
    
    // Demonstrate cross-node access using DFlow
    auto nodes = controller->get_all_nodes();
    if (nodes.size() >= 2) {
        std::cout << "\n=== Testing Cross-Node Access with DFlow ===" << std::endl;
        
        // Execute sproc on node that doesn't have the data
        StorageNode* executor_node = nodes[0];
        
        try {
            auto result = executor_node->execute_sproc(gp2.address, gp2.size, StoredProcedures::sum_array);
            std::cout << "Aggregation result: " << result << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << std::endl;
        }
        
        controller->print_status();
    }
    
    // Simulate spot instance reclamation
    std::cout << "\n=== Simulating Spot Instance Reclamation ===" << std::endl;
    std::thread reclamation_thread([&nodes]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!nodes.empty() && nodes[0]->get_info().is_spot_instance) {
            nodes[0]->simulate_reclamation();
        }
    });
    
    reclamation_thread.join();
    controller->print_status();
    
    std::cout << "Demo completed!" << std::endl;
}

int main() {
    try {
        run_compucache_demo();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}