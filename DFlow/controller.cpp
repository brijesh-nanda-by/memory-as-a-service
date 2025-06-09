#include "httplib.h" // For HTTP server
#include <nlohmann/json.hpp>  // For JSON parsing and generation

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm> // For std::remove_if, std::sort
#include <mutex>     // For std::mutex
#include <chrono>    // For std::chrono
#include <thread>    // For std::thread

// Use nlohmann/json for convenience
using json = nlohmann::json;

// --- Data Structures for Controller State ---

// Represents information about a registered worker node
struct NodeInfo {
    std::string id;
    std::string ip;
    int port;
    long long last_heartbeat_timestamp; // Unix timestamp in seconds
};

// --- Global Controller State ---
// Mutex to protect shared data structures
std::mutex g_mutex;
// Map to store registered nodes: node_id -> NodeInfo
std::unordered_map<std::string, NodeInfo> g_node_registry;
// List of active node IDs for consistent hashing (simplified)
std::vector<std::string> g_active_node_ids;

// --- Consistent Hashing Logic (Simplified) ---
// IMPORTANT: This is a very basic modulo-based distribution for demonstration.
// For a production-grade distributed system, you would implement a robust
// consistent hashing algorithm (e.g., Rendezvous Hashing, or a ring-based DHT).
// This ensures that when nodes are added or removed, only a small fraction
// of data needs to be remapped, minimizing data movement.
class ConsistentHasher {
public:
    // Updates the list of active nodes.
    // Call this under a lock whenever g_active_node_ids changes.
    void update_nodes(const std::vector<std::string>& node_ids) {
        m_nodes = node_ids;
        std::sort(m_nodes.begin(), m_nodes.end()); // Keep sorted for simple lookup
    }

    // Determines which node should store a given key.
    // Returns the ID of the responsible node.
    std::string get_node_for_key(const std::string& key) const {
        if (m_nodes.empty()) {
            return ""; // No nodes available
        }
        // Use std::hash for string, then modulo by number of active nodes
        // Note: std::hash is implementation-defined and not good for real distribution.
        size_t hash_val = std::hash<std::string>{}(key);
        return m_nodes[hash_val % m_nodes.size()];
    }

private:
    std::vector<std::string> m_nodes; // Current list of active node IDs
};

ConsistentHasher g_hasher;

// --- Controller Server Endpoints ---

// Handles node registration requests from worker nodes.
void handle_register_node(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_mutex); // Protect shared data

    try {
        auto data = json::parse(req.body);
        NodeInfo node;
        node.id = data["node_id"].get<std::string>();
        node.ip = data["ip"].get<std::string>();
        node.port = data["port"].get<int>();
        node.last_heartbeat_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Add/update node in registry
        g_node_registry[node.id] = node;

        // Update active node IDs for hashing
        // First, check if it's a new node
        bool found = false;
        for (const auto& existing_id : g_active_node_ids) {
            if (existing_id == node.id) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_active_node_ids.push_back(node.id);
            g_hasher.update_nodes(g_active_node_ids); // Re-calculate distribution
        }
        
        std::cout << "[Controller] Node " << node.id << " registered/updated: " 
                  << node.ip << ":" << node.port << std::endl;

        res.status = 200;
        res.set_content(json{{"status", "registered"}}.dump(), "application/json");
    } catch (const json::parse_error& e) {
        res.status = 400;
        res.set_content(json{{"error", "Invalid JSON format"}}.dump(), "application/json");
        std::cerr << "[Controller Error] JSON parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json{{"error", std::string("Server error: ") + e.what()}}.dump(), "application/json");
        std::cerr << "[Controller Error] Unhandled exception in /register_node: " << e.what() << std::endl;
    }
}

// Handles heartbeat requests from worker nodes.
void handle_heartbeat(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_mutex); // Protect shared data

    try {
        auto data = json::parse(req.body);
        std::string node_id = data["node_id"].get<std::string>();

        if (g_node_registry.count(node_id)) {
            g_node_registry[node_id].last_heartbeat_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            // std::cout << "[Controller] Heartbeat from " << node_id << std::endl; // Too verbose
            res.status = 200;
            res.set_content(json{{"status", "ok"}}.dump(), "application/json");
        } else {
            res.status = 404;
            res.set_content(json{{"status", "node not recognized"}}.dump(), "application/json");
        }
    } catch (const json::parse_error& e) {
        res.status = 400;
        res.set_content(json{{"error", "Invalid JSON format"}}.dump(), "application/json");
        std::cerr << "[Controller Error] JSON parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json{{"error", std::string("Server error: ") + e.what()}}.dump(), "application/json");
        std::cerr << "[Controller Error] Unhandled exception in /heartbeat: " << e.what() << std::endl;
    }
}

// Provides the IP and Port of the node responsible for a given key.
void handle_get_node_for_key(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(g_mutex); // Protect shared data

    std::string key = req.path.substr(req.path.find_last_of('/') + 1); // Extract key from URL

    std::string target_node_id = g_hasher.get_node_for_key(key);

    if (!target_node_id.empty() && g_node_registry.count(target_node_id)) {
        const NodeInfo& node_info = g_node_registry[target_node_id];
        json response_json = {
            {"node_id", node_info.id},
            {"ip", node_info.ip},
            {"port", node_info.port}
        };
        res.status = 200;
        res.set_content(response_json.dump(), "application/json");
    } else {
        res.status = 404;
        res.set_content(json{{"error", "No active node found for key or node is offline"}}.dump(), "application/json");
    }
}

// --- Background Task: Node Health Monitoring ---
void monitor_node_health(long long timeout_seconds) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds / 2)); // Check periodically

        std::lock_guard<std::mutex> lock(g_mutex); // Protect shared data

        long long current_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        bool nodes_removed = false;
        // Remove inactive nodes and update g_active_node_ids
        for (auto it = g_node_registry.begin(); it != g_node_registry.end(); ) {
            if (current_time - it->second.last_heartbeat_timestamp > timeout_seconds) {
                std::cout << "[Controller] Node " << it->first << " timed out. Removing." << std::endl;
                // Remove from g_active_node_ids
                g_active_node_ids.erase(
                    std::remove(g_active_node_ids.begin(), g_active_node_ids.end(), it->first),
                    g_active_node_ids.end()
                );
                it = g_node_registry.erase(it); // Remove from registry
                nodes_removed = true;
            } else {
                ++it;
            }
        }

        if (nodes_removed) {
            g_hasher.update_nodes(g_active_node_ids); // Re-calculate distribution if nodes removed
        }
    }
}

// --- Main Controller Function ---
int main(int argc, char* argv[]) {
    // Default controller port
    int controller_port = 5000;
    // Heartbeat timeout: if a node doesn't send a heartbeat for this long, it's considered offline
    long long heartbeat_timeout_seconds = 15; 

    if (argc > 1) {
        controller_port = std::stoi(argv[1]);
    }

    httplib::Server svr;

    // Set up request handlers
    svr.Post("/register_node", handle_register_node);
    svr.Post("/heartbeat", handle_heartbeat);
    svr.Get(R"(/get_node_for_key/(.*))", handle_get_node_for_key); // Regex to capture key

    // Start health monitoring in a separate thread
    std::thread monitor_thread(monitor_node_health, heartbeat_timeout_seconds);
    monitor_thread.detach(); // Detach to run in background

    std::cout << "[Controller] Starting on port " << controller_port << "..." << std::endl;
    svr.listen("0.0.0.0", controller_port); // Listen on all interfaces

    return 0;
}

