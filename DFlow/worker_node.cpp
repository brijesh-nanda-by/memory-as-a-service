#include "httplib.h" // For HTTP server and client
#include <nlohmann/json.hpp>  // For JSON parsing and generation

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>     // For std::mutex
#include <chrono>    // For std::chrono
#include <thread>    // For std::thread
#include <random>    // For std::random_device, std::mt19937, std::uniform_int_distribution
#include <sstream>   // For std::stringstream

// Use nlohmann/json for convenience
using json = nlohmann::json;

// --- Global Worker Node State ---
std::string g_node_id;
std::string g_node_ip; // Default IP for this node
int g_node_port;                     // Port for this node
std::string g_controller_url = "http://amd235.utah.cloudlab.us:5000"; // Controller's URL

// Mutex to protect the local hashmap
std::mutex g_map_mutex;
// The in-memory hashmap for this specific node
std::unordered_map<std::string, std::string> g_local_hashmap;

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
                  << res.error() << std::endl;
    }
}

// Sends periodic heartbeats to the Controller.
void send_heartbeat() {
    httplib::Client cli(g_controller_url);
    json req_body = {{"node_id", g_node_id}};

    while (true) {
        if (auto res = cli.Post("/heartbeat", req_body.dump(), "application/json")) {
            // Heartbeat successful
        } else {
            std::cerr << "[Node " << g_node_id << "] Error sending heartbeat: " 
                      << res.error() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Heartbeat every 5 seconds
    }
}

// Queries the Controller to find which node is responsible for a given key.
// Returns a json object with node_id, ip, port, or an empty json if not found.
json get_node_info_for_key(const std::string& key) {
    httplib::Client cli(g_controller_url);
    if (auto res = cli.Get("/get_node_for_key/" + key)) {
        if (res->status == 200) {
            return json::parse(res->body);
        } else {
            std::cerr << "[Node " << g_node_id << "] Controller lookup failed for key " << key 
                      << ": " << res->status << " " << res->body << std::endl;
        }
    } else {
        std::cerr << "[Node " << g_node_id << "] Error connecting to Controller for key lookup: " 
                  << res.error() << std::endl;
    }
    return json(); // Return empty json on error
}

// --- Worker Node Server Endpoints ---

// Handles PUT requests (store a key-value pair).
void handle_put(const httplib::Request& req, httplib::Response& res) {
    std::string key = req.path.substr(req.path.find_last_of('/') + 1);
    
    try {
        auto data = json::parse(req.body);
        std::string value = data["value"].get<std::string>();

        json target_node_info = get_node_info_for_key(key);
        if (target_node_info.empty()) {
            res.status = 500;
            res.set_content(json{{"error", "Could not determine target node for key"}}.dump(), "application/json");
            return;
        }

        std::string target_node_id = target_node_info["node_id"].get<std::string>();

        if (target_node_id == g_node_id) {
            // This node is responsible for the key, store locally
            std::lock_guard<std::mutex> lock(g_map_mutex); // Protect local hashmap
            g_local_hashmap[key] = value;
            std::cout << "[Node " << g_node_id << "] Stored '" << key << "':'" << value << "' locally." << std::endl;
            res.status = 200;
            res.set_content(json{{"status", "stored locally"}}.dump(), "application/json");
        } else {
            // Forward the request to the correct node
            std::string target_ip = target_node_info["ip"].get<std::string>();
            int target_port = target_node_info["port"].get<int>();

            std::cout << "[Node " << g_node_id << "] Forwarding PUT '" << key << "' to " 
                      << target_node_id << " (" << target_ip << ":" << target_port << ")" << std::endl;

            httplib::Client cli(target_ip, target_port);
            json forward_body = {{"value", value}};
            if (auto forward_res = cli.Post("/put/" + key, forward_body.dump(), "application/json")) {
                res.status = forward_res->status;
                res.set_content(forward_res->body, forward_res->get_header_value("Content-Type"));
            } else {
                res.status = 500;
                res.set_content(json{{"error", std::string("Failed to forward PUT request: ") + to_string(forward_res.error())}}.dump(), "application/json");
                std::cerr << "[Node " << g_node_id << "] Error forwarding PUT: " << to_string(forward_res.error()) << std::endl;
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
        json target_node_info = get_node_info_for_key(key);
        if (target_node_info.empty()) {
            res.status = 500;
            res.set_content(json{{"error", "Could not determine target node for key"}}.dump(), "application/json");
            return;
        }

        std::string target_node_id = target_node_info["node_id"].get<std::string>();

        if (target_node_id == g_node_id) {
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
            std::string target_ip = target_node_info["ip"].get<std::string>();
            int target_port = target_node_info["port"].get<int>();

            std::cout << "[Node " << g_node_id << "] Forwarding GET '" << key << "' to " 
                      << target_node_id << " (" << target_ip << ":" << target_port << ")" << std::endl;

            httplib::Client cli(target_ip, target_port);
            if (auto forward_res = cli.Get("/get/" + key)) {
                res.status = forward_res->status;
                res.set_content(forward_res->body, forward_res->get_header_value("Content-Type"));
            } else {
                res.status = 500;
                res.set_content(json{{"error", std::string("Failed to forward GET request: ") + to_string(forward_res.error())}}.dump(), "application/json");
                std::cerr << "[Node " << g_node_id << "] Error forwarding GET: " << to_string(forward_res.error()) << std::endl;
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
    if (argc > 2) { // Optional: Controller IP/Port can be passed as second arg
        g_controller_url = argv[2];
    }

    httplib::Server svr;

    // Set up request handlers for this node
    svr.Post(R"(/put/(.*))", handle_put); // Regex to capture key
    svr.Get(R"(/get/(.*))", handle_get);   // Regex to capture key

    // Register with controller on startup
    // This is done once, blocking until registration is attempted
    register_with_controller();
    
    // Start heartbeat thread
    std::thread heartbeat_thread(send_heartbeat);
    heartbeat_thread.detach(); // Detach to run in background

    std::cout << "[Node " << g_node_id << "] Starting on " 
              << g_node_ip << ":" << g_node_port << "..." << std::endl;
    
    // Listen on all interfaces
    svr.listen(g_node_ip.c_str(), g_node_port); 

    return 0;
}
