#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <sys/un.h>

// Message header sent over the Unix socket
struct unix_message_header {
    uint32_t length;     // Length of the payload following this header
    uint32_t type;       // Application-defined message type
} __attribute__((packed));

// Per-client state
struct unix_client {
    int fd;
    // Partial read buffer for reassembling framed messages
    std::vector<uint8_t> recv_buf;
};

// Callback invoked when a complete message is received from a client.
// Parameters: client fd, message type, payload pointer, payload length
using unix_message_cb = std::function<void(int client_fd, uint32_t type, const uint8_t* payload, size_t len)>;

// Callback invoked when a client connects. Parameter: client fd
using unix_connect_cb = std::function<void(int client_fd)>;

// Callback invoked when a client disconnects. Parameter: client fd
using unix_disconnect_cb = std::function<void(int client_fd)>;

class UnixServer
{
private:
    int listen_fd{-1};
    std::string socket_path;
    std::vector<unix_client> clients;

    unix_message_cb on_message;
    unix_connect_cb on_connect;
    unix_disconnect_cb on_disconnect;

    // Process any complete framed messages in a client's recv buffer
    void process_client_buffer(unix_client& client);

public:
    UnixServer(const std::string& path);
    ~UnixServer();

    // No copy
    UnixServer(const UnixServer&) = delete;
    UnixServer& operator=(const UnixServer&) = delete;

    // Start listening on the Unix socket
    bool start();

    // Stop the server and disconnect all clients
    void stop();

    // Register callbacks
    void set_on_message(unix_message_cb cb)       { on_message = std::move(cb); }
    void set_on_connect(unix_connect_cb cb)        { on_connect = std::move(cb); }
    void set_on_disconnect(unix_disconnect_cb cb)   { on_disconnect = std::move(cb); }

    // Return the listening socket fd (for adding to poll set)
    int get_listen_fd() const { return listen_fd; }

    // Return fds of all connected clients (for adding to poll set)
    std::vector<int> get_client_fds() const;

    // Call when poll indicates the listen fd is readable — accepts a new client
    void accept_client();

    // Call when poll indicates a client fd is readable — reads data and
    // invokes the message callback for each complete message
    void handle_client_data(int client_fd);

    // Send a framed message to a specific client
    bool send_to_client(int client_fd, uint32_t type, const uint8_t* payload, size_t len);

    // Broadcast a framed message to all connected clients
    void broadcast(uint32_t type, const uint8_t* payload, size_t len);

    // Number of connected clients
    size_t client_count() const { return clients.size(); }
};
