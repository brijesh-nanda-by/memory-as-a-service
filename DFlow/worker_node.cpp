#include "httplib.h"
#include <nlohmann/json.hpp> // Corrected include path
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm> // For std::sort, std::find
#include <mutex>     // For std::mutex
#include <chrono>    // For std::chrono
#include <thread>    // For std::thread
#include <random>    // For std::random_device, std::mt19937, std::uniform_int_distribution
#include <sstream>   // For std::stringstream
#include <stdexcept> // For std::runtime_error

using json = nlohmann::json;

// --- Data Structures for Worker Node State ---

// // NodeInfo struct (must match Controller's definition)
// struct NodeInfo {
//     std::string id;
//     std::string ip;
//     int port;

//     // Helper to create NodeInfo from JSON (for parsing Controller's response)
//     static NodeInfo from_json(const json& j) {
//         NodeInfo n;
//         n.id = j["node_id"].get<std::string>();
//         n.ip = j["ip"].get<std::string>();
//         n.port = j["port"].get<int>();
//         return n;
//     }
// };

struct NodeInfo
{
    std::string id;
    std::string ip;
    int port;

    static NodeInfo from_json(json j) {
        NodeInfo n;
        n.id = j["node_id"].get<std::string>();
        n.ip = j["ip"].get<std::string>();
        n.port = j["port"].get<int>();
        return n;
    }

};


// --- Consistent Hashing Logic ---
class ConsistentHasher {
public:
    // Updates the list of active nodes for hashing.
    // This list should be sorted by node ID to ensure deterministic behavior
    // across all workers using the same set of nodes.
    void update_nodes(const std::vector<NodeInfo>& nodes_info) {
        // Create a sorted list of node IDs for consistent hashing
        m_nodes.clear();
        for (const auto& node : nodes_info) {
            m_nodes.push_back(node.id);
            // Also store NodeInfo mapping for direct lookup later
            m_node_id_to_info_map[node.id] = node;
        }
        std::sort(m_nodes.begin(), m_nodes.end()); // Keep node IDs sorted

        // Debugging: Print current active nodes
        // std::cout << "Hasher updated. Active nodes (" << m_nodes.size() << "): ";
        // for(const auto& id : m_nodes) std::cout << id.substr(0,8) << "... ";
        // std::cout << std::endl;
    }

    // Determines which node ID should store a given key.
    // Returns the ID of the responsible node.
    std::string get_owning_node_id(const std::string& key) const {
        if (m_nodes.empty()) {
            return ""; // No nodes available
        }
        // Basic modulo hash.
        // A cryptographic hash (e.g., SHA256) would be more robust for distribution.
        size_t hash_val = std::hash<std::string>{}(key);
        return m_nodes[hash_val % m_nodes.size()];
    }

    // Retrieves NodeInfo for a given node ID from its internal map.
    NodeInfo get_node_info(const std::string& node_id) const {
        auto it = m_node_id_to_info_map.find(node_id);
        if (it != m_node_id_to_info_map.end()) {
            return it->second;
        }
        throw std::runtime_error("NodeInfo not found for ID: " + node_id);
    }

private:
    // Sorted list of active node IDs used for the hashing distribution.
    std::vector<std::string> m_nodes;
    // Map to quickly get NodeInfo by ID once the owning node ID is determined.
    std::unordered_map<std::string, NodeInfo> m_node_id_to_info_map;
};


// --- Global Worker Node State ---
std::string g_node_id;
std::string g_node_ip;                     // This will be discovered via external API
int g_node_port;
std::string g_controller_url = "http://amd235.utah.cloudlab.us:5000"; // Controller's URL

std::mutex g_map_mutex; // Mutex to protect the local hashmap
std::unordered_map<std::string, std::string> g_local_hashmap;

std::mutex g_hasher_mutex; // Mutex to protect the ConsistentHasher instance
ConsistentHasher g_hasher; // Instance of the hasher for local key determination

// --- Helper Functions ---

// Generates a UUID for the node ID (simplified)
std::string generate_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 15);

    const char* v = "0123456789abcdef";
    const bool dash[] = { 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0 };

    std::stringstream ss;
    for (int i = 0; i < 16; i++) {
        if (dash[i]) ss << "-";
        ss << v[distrib(gen)];
        ss << v[distrib(gen)];
    }
    return ss.str();
}

// Function to retrieve the public IP address from an external service.
// (Copy/Paste this from the previous recommended snippet for external IP)
std::string get_public_ip_address() {
    const std::string ip_service_url = "http://icanhazip.com";
    httplib::Client cli(ip_service_url.c_str());
    if (auto res = cli.Get("/")) {
        if (res->status == 200) {
            std::string public_ip = res->body;
            public_ip.erase(0, public_ip.find_first_not_of(" \n\r\t"));
            public_ip.erase(public_ip.find_last_not_of(" \n\r\t") + 1);
            return public_ip;
        } else {
            std::cerr << "[Node " << g_node_id << "] Failed to get public IP from service: HTTP status code " << res->status << std::endl;
        }
    } else {
        std::cerr << "[Node " << g_node_id << "] Failed to connect to IP service for public IP: " << httplib::to_string(res.error()) << std::endl;
    }
    return ""; // Return empty string on failure
}

// --- Communication with Controller ---

// Registers this worker node with the central Controller.
void register_with_controller() {
    httplib::Client cli(g_controller_url);
    json req_body = {
        {"node_id", g_node_id},
        {"ip", g_node_ip},
        {"port", g_node_port}
    };

    if (auto res = cli.Post("/register_node", req_body.dump(), "application/json")) {
        if (res->status == 200) {
            std::cout << "[Node " << g_node_id << "] Successfully registered with Controller." << std::endl;
        } else {
            std::cerr << "[Node " << g_node_id << "] Failed to register with Controller: " 
                      << res->status << " " << res->body << std::endl;
        }
    } else {
        std::cerr << "[Node " << g_node_id << "] Error connecting to Controller for registration: " 
                  << httplib::to_string(res.error()) << std::endl;
    }
}

// Sends periodic heartbeats to the Controller.
void send_heartbeat() {
    httplib::Client cli(g_controller_url);
    json req_body = {{"node_id", g_node_id}};

    while (true) {
        if (auto res = cli.Post("/heartbeat", req_body.dump(), "application/json")) {
            if (res->status == 404) { // Controller says node not recognized
                std::cerr << "[Node " << g_node_id << "] Heartbeat: Controller says node not recognized, attempting re-registration." << std::endl;
                register_with_controller(); // Re-register
            }
        } else {
            std::cerr << "[Node " << g_node_id << "] Error sending heartbeat: " 
                      << httplib::to_string(res.error()) << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Heartbeat every 5 seconds
    }
}

// FUNCTION: Periodically fetches the list of active nodes from the Controller
// and updates the local ConsistentHasher.
void update_active_nodes_list_periodically() {
    httplib::Client cli(g_controller_url);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(120)); // Update every 120 seconds

        if (auto res = cli.Get("/get_active_nodes")) {
            if (res->status == 200) {
                try {
                    auto json_array = json::parse(res->body);
                    std::vector<NodeInfo> active_nodes;
                    for (const auto& node_json : json_array) {
                        active_nodes.push_back(NodeInfo::from_json(node_json));
                    }
                    
                    std::lock_guard<std::mutex> lock(g_hasher_mutex); // Protect hasher updates
                    g_hasher.update_nodes(active_nodes);
                    // std::cout << "[Node " << g_node_id << "] Active nodes list updated. Current count: " 
                    //           << active_nodes.size() << std::endl; // Can be verbose
                } catch (const json::parse_error& e) {
                    std::cerr << "[Node " << g_node_id << "] Error parsing active nodes JSON: " << e.what() << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[Node " << g_node_id << "] Unhandled error updating active nodes: " << e.what() << std::endl;
                }
            } else {
                std::cerr << "[Node " << g_node_id << "] Failed to get active nodes from Controller: " 
                          << res->status << " " << res->body << std::endl;
            }
        } else {
            std::cerr << "[Node " << g_node_id << "] Error connecting to Controller for active nodes: " 
                      << httplib::to_string(res.error()) << std::endl;
        }
    }
}


// --- Worker Node Server Endpoints ---

// Handles PUT requests (store a key-value pair).
void handle_put(const httplib::Request& req, httplib::Response& res) {
    std::string key = req.path.substr(req.path.find_last_of('/') + 1);
    
    try {
        auto data = json::parse(req.body);
        std::string value = data["value"].get<std::string>();

        std::string owning_node_id;
        NodeInfo owning_node_info;

        {
            std::lock_guard<std::mutex> lock(g_hasher_mutex); // Protect hasher access
            owning_node_id = g_hasher.get_owning_node_id(key);
            owning_node_info = g_hasher.get_node_info(owning_node_id);
        }

        if (owning_node_id == g_node_id) {
            // This node is responsible for the key, store locally
            std::lock_guard<std::mutex> lock(g_map_mutex); // Protect local hashmap
            g_local_hashmap[key] = value;
            std::cout << "[Node " << g_node_id << "] Stored '" << key << "':'" << value << "' locally." << std::endl;
            res.status = 200;
            res.set_content(json{{"status", "stored locally"}}.dump(), "application/json");
        } else {
            // Forward the request to the correct node
            std::string target_ip = owning_node_info.ip;
            int target_port = owning_node_info.port;

            std::cout << "[Node " << g_node_id << "] Forwarding PUT '" << key << "' to " 
                      << owning_node_id.substr(0,8) << "... (" << target_ip << ":" << target_port << ")" << std::endl;

            httplib::Client cli(target_ip, target_port);
            json forward_body = {{"value", value}};
            if (auto forward_res = cli.Post("/put/" + key, forward_body.dump(), "application/json")) {
                res.status = forward_res->status;
                res.set_content(forward_res->body, forward_res->get_header_value("Content-Type"));
            } else {
                res.status = 500;
                res.set_content(json{{"error", std::string("Failed to forward PUT request: ") + httplib::to_string(forward_res.error())}}.dump(), "application/json");
                std::cerr << "[Node " << g_node_id << "] Error forwarding PUT: " << httplib::to_string(forward_res.error()) << std::endl;
            }
        }
    } catch (const json::parse_error& e) {
        res.status = 400;
        res.set_content(json{{"error", "Invalid JSON format"}}.dump(), "application/json");
        std::cerr << "[Node " << g_node_id << " Error] JSON parse error in PUT: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json{{"error", std::string("Server error in PUT: ") + e.what()}}.dump(), "application/json");
        std::cerr << "[Node " << g_node_id << " Error] Unhandled exception in PUT: " << e.what() << std::endl;
    }
}

// Handles GET requests (retrieve a value for a key).
void handle_get(const httplib::Request& req, httplib::Response& res) {
    std::string key = req.path.substr(req.path.find_last_of('/') + 1);

    try {
        std::string owning_node_id;
        NodeInfo owning_node_info;

        {
            std::lock_guard<std::mutex> lock(g_hasher_mutex); // Protect hasher access
            owning_node_id = g_hasher.get_owning_node_id(key);
            owning_node_info = g_hasher.get_node_info(owning_node_id);
        }

        if (owning_node_id == g_node_id) {
            // This node is responsible for the key, retrieve locally
            std::lock_guard<std::mutex> lock(g_map_mutex); // Protect local hashmap
            auto it = g_local_hashmap.find(key);
            if (it != g_local_hashmap.end()) {
                std::string value = it->second;
                std::cout << "[Node " << g_node_id << "] Retrieved '" << key << "':'" << value << "' locally." << std::endl;
                res.status = 200;
                res.set_content(json{{"key", key}, {"value", value}}.dump(), "application/json");
            } else {
                res.status = 404;
                res.set_content(json{{"error", "Key not found locally"}}.dump(), "application/json");
            }
        } else {
            // Forward the request to the correct node
            std::string target_ip = owning_node_info.ip;
            int target_port = owning_node_info.port;

            std::cout << "[Node " << g_node_id << "] Forwarding GET '" << key << "' to " 
                      << owning_node_id.substr(0,8) << "... (" << target_ip << ":" << target_port << ")" << std::endl;

            httplib::Client cli(target_ip, target_port);
            if (auto forward_res = cli.Get("/get/" + key)) {
                res.status = forward_res->status;
                res.set_content(forward_res->body, forward_res->get_header_value("Content-Type"));
            } else {
                res.status = 500;
                res.set_content(json{{"error", std::string("Failed to forward GET request: ") + httplib::to_string(forward_res.error())}}.dump(), "application/json");
                std::cerr << "[Node " << g_node_id << "] Error forwarding GET: " << httplib::to_string(forward_res.error()) << std::endl;
            }
        }
    } catch (const json::parse_error& e) {
        res.status = 400;
        res.set_content(json{{"error", "Invalid JSON format"}}.dump(), "application/json");
        std::cerr << "[Node " << g_node_id << " Error] JSON parse error in GET: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json{{"error", std::string("Server error in GET: ") + e.what()}}.dump(), "application/json");
        std::cerr << "[Node " << g_node_id << " Error] Unhandled exception in GET: " << e.what() << std::endl;
    }
}

std::string get_public_ip_address() {
    // The URL of the external service that returns the public IP.
    // Using http://icanhazip.com for simplicity.
    const std::string ip_service_url = "http://icanhazip.com";

    // Create an httplib client instance targeting the IP service.
    httplib::Client cli(ip_service_url.c_str());

    // Attempt to make a GET request to the root path ("/") of the service.
    if (auto res = cli.Get("/")) {
        // Check if the HTTP request was successful (status code 200 OK).
        if (res->status == 200) {
            // The response body contains the public IP address.
            // It might include leading/trailing whitespace or newlines, so we trim them.
            std::string public_ip = res->body;
            public_ip.erase(0, public_ip.find_first_not_of(" \n\r\t")); // Trim leading whitespace/newlines
            public_ip.erase(public_ip.find_last_not_of(" \n\r\t") + 1); // Trim trailing whitespace/newlines
            return public_ip; // Return the cleaned public IP
        } else {
            // Log an error if the HTTP request was not successful.
            std::cerr << "Failed to get public IP from service: HTTP status code " << res->status << std::endl;
        }
    } else {
        // Log an error if there was a problem connecting to the IP service (e.g., network issue).
        std::cerr << "Failed to connect to IP service: " << httplib::to_string(res.error()) << std::endl;
    }
    return ""; // Return an empty string on any failure
}

// --- Main Worker Node Function ---
int main(int argc, char* argv[]) {
    g_node_id = generate_uuid(); // Generate unique ID for this node
    g_node_ip = get_public_ip_address(); // Get public IP address of this node
    // Default node port (can be overridden by command line arg)
    g_node_port = 5001; 
    if (argc > 1) {
        g_node_port = std::stoi(argv[1]);
    }
    // Optional: Controller URL can be passed as a second argument
    if (argc > 2) {
        g_controller_url = argv[2];
    }

    // --- Discover Public IP Address ---
    // This part should be kept to ensure the node registers with its public IP
    // so other nodes/clients can connect to it properly.
    std::cout << "[Node " << g_node_id << "] Attempting to discover public IP address..." << std::endl;
    std::string detected_public_ip = get_public_ip_address();

    if (!detected_public_ip.empty()) {
        g_node_ip = detected_public_ip;
        std::cout << "[Node " << g_node_id << "] Successfully detected public IP: " << g_node_ip << std::endl;
    } else {
        std::cerr << "[Node " << g_node_id << "] Warning: Could not detect public IP. "
                  << "Node might not be reachable externally if firewall/NAT is involved. "
                  << "Using default/internal fallback IP: " << g_node_ip << std::endl;
        // If public IP detection fails, g_node_ip remains "127.0.0.1" from its default initialization.
        // This means it might only be accessible locally or via SSH tunnels if not public.
    }
    // ----------------------------------

    // Register with controller on startup
    register_with_controller();

    // Start background threads
    std::thread heartbeat_thread(send_heartbeat);
    heartbeat_thread.detach();

    std::thread active_nodes_update_thread(update_active_nodes_list_periodically);
    active_nodes_update_thread.detach();

    std::cout << "[Node " << g_node_id << "] Starting server on "
              << g_node_ip << ":" << g_node_port << " (binding to 0.0.0.0 for external access)..." << std::endl;

    // Set up httplib server request handlers
    httplib::Server svr;
    svr.Post(R"(/put/(.*))", handle_put);
    svr.Get(R"(/get/(.*))", handle_get);

    // Listen on all interfaces to allow external connections (binding to "0.0.0.0").
    // The IP reported to the controller (g_node_ip) is for other nodes/clients to use.
    svr.listen("0.0.0.0", g_node_port);

    return 0;
}
