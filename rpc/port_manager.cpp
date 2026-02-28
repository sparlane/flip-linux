#include <algorithm>

#include "rpc_port_manager.hpp"

bool RpcPortManager::register_local_port(const rpc_port_t& port, int client_fd, std::string unix_socket)
{
    auto it = local_ports.find(port);
    if (it != local_ports.end()) {
        return it->second.client_fd == client_fd;
    }

    local_ports.emplace(port, RpcPortBinding{client_fd, std::move(unix_socket)});
    return true;
}

bool RpcPortManager::unregister_local_port(const rpc_port_t& port, int client_fd)
{
    auto it = local_ports.find(port);
    if (it == local_ports.end()) {
        return false;
    }

    if (it->second.client_fd != client_fd) {
        return false;
    }

    local_ports.erase(it);
    return true;
}

void RpcPortManager::remove_client(int client_fd)
{
    for (auto it = local_ports.begin(); it != local_ports.end(); ) {
        if (it->second.client_fd == client_fd) {
            it = local_ports.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = pending_lookups.begin(); it != pending_lookups.end(); ) {
        auto& requests = it->second;
        requests.erase(
            std::remove_if(requests.begin(), requests.end(),
                [client_fd](const RpcLookupRequest& req) { return req.client_fd == client_fd; }),
            requests.end());

        if (requests.empty()) {
            it = pending_lookups.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<RpcPortBinding> RpcPortManager::get_local_binding(const rpc_port_t& port) const
{
    auto it = local_ports.find(port);
    if (it == local_ports.end()) {
        return std::nullopt;
    }

    return it->second;
}

void RpcPortManager::begin_remote_lookup(const rpc_port_t& port, int client_fd, lookup_cb cb)
{
    pending_lookups[port].push_back(RpcLookupRequest{client_fd, std::move(cb)});
}

void RpcPortManager::resolve_remote_lookup(const rpc_port_t& port, const std::string& remote_socket, bool found)
{
    auto it = pending_lookups.find(port);
    if (it == pending_lookups.end()) {
        return;
    }

    auto requests = std::move(it->second);
    pending_lookups.erase(it);

    for (auto& req : requests) {
        if (req.callback) {
            req.callback(req.client_fd, port, remote_socket, found);
        }
    }
}

size_t RpcPortManager::pending_lookup_count() const
{
    size_t total = 0;
    for (const auto& entry : pending_lookups) {
        total += entry.second.size();
    }
    return total;
}
