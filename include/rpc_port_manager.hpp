#pragma once
#include <array>
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

using rpc_port_t = std::array<uint8_t, 6>;

struct RpcPortLess {
    bool operator()(const rpc_port_t& lhs, const rpc_port_t& rhs) const
    {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }
};

struct RpcPortBinding {
    int client_fd{-1};
    std::string unix_socket;
};

class RpcPortManager
{
public:
    using lookup_cb = std::function<void(int client_fd, const rpc_port_t& port, const std::string& remote_socket, bool found)>;

    bool register_local_port(const rpc_port_t& port, int client_fd, std::string unix_socket = {});
    bool unregister_local_port(const rpc_port_t& port, int client_fd);
    void remove_client(int client_fd);

    std::optional<RpcPortBinding> get_local_binding(const rpc_port_t& port) const;

    void begin_remote_lookup(const rpc_port_t& port, int client_fd, lookup_cb cb);
    void resolve_remote_lookup(const rpc_port_t& port, const std::string& remote_socket, bool found);

    size_t local_port_count() const { return local_ports.size(); }
    size_t pending_lookup_count() const;

private:
    struct RpcLookupRequest {
        int client_fd{-1};
        lookup_cb callback;
    };

    std::map<rpc_port_t, RpcPortBinding, RpcPortLess> local_ports;
    std::map<rpc_port_t, std::vector<RpcLookupRequest>, RpcPortLess> pending_lookups;
};
