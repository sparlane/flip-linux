#include <iostream>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <errno.h>

#include "unix_server.hpp"

UnixServer::UnixServer(const std::string& path)
    : socket_path(path)
{
}

UnixServer::~UnixServer()
{
    stop();
}

bool UnixServer::start()
{
    // Remove stale socket file if it exists
    unlink(socket_path.c_str());

    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        std::cerr << "UnixServer: socket() failed: " << strerror(errno) << std::endl;
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "UnixServer: socket path too long" << std::endl;
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "UnixServer: bind() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        listen_fd = -1;
        return false;
    }

    if (listen(listen_fd, 8) < 0) {
        std::cerr << "UnixServer: listen() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        listen_fd = -1;
        unlink(socket_path.c_str());
        return false;
    }

    std::cout << "UnixServer: listening on " << socket_path << std::endl;
    return true;
}

void UnixServer::stop()
{
    for (auto& client : clients) {
        close(client.fd);
    }
    clients.clear();

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
        unlink(socket_path.c_str());
    }
}

std::vector<int> UnixServer::get_client_fds() const
{
    std::vector<int> fds;
    fds.reserve(clients.size());
    for (const auto& c : clients) {
        fds.push_back(c.fd);
    }
    return fds;
}

void UnixServer::accept_client()
{
    int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "UnixServer: accept4() failed: " << strerror(errno) << std::endl;
        }
        return;
    }

    clients.push_back({client_fd, {}});
    std::cout << "UnixServer: client connected (fd=" << client_fd << ")" << std::endl;

    if (on_connect) {
        on_connect(client_fd);
    }
}

void UnixServer::handle_client_data(int client_fd)
{
    auto it = std::find_if(clients.begin(), clients.end(),
        [client_fd](const unix_client& c) { return c.fd == client_fd; });

    if (it == clients.end()) {
        return;
    }

    uint8_t tmp[4096];
    ssize_t n = read(client_fd, tmp, sizeof(tmp));

    if (n <= 0) {
        // Client disconnected or error
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; // Spurious wakeup
        }
        std::cout << "UnixServer: client disconnected (fd=" << client_fd << ")" << std::endl;
        close(client_fd);
        if (on_disconnect) {
            on_disconnect(client_fd);
        }
        clients.erase(it);
        return;
    }

    it->recv_buf.insert(it->recv_buf.end(), tmp, tmp + n);
    process_client_buffer(*it);
}

void UnixServer::process_client_buffer(unix_client& client)
{
    while (client.recv_buf.size() >= sizeof(unix_message_header)) {
        unix_message_header hdr;
        std::memcpy(&hdr, client.recv_buf.data(), sizeof(hdr));

        size_t total = sizeof(unix_message_header) + hdr.length;
        if (client.recv_buf.size() < total) {
            break; // Wait for more data
        }

        if (on_message) {
            const uint8_t* payload = client.recv_buf.data() + sizeof(unix_message_header);
            on_message(client.fd, hdr.type, payload, hdr.length);
        }

        client.recv_buf.erase(client.recv_buf.begin(), client.recv_buf.begin() + total);
    }
}

bool UnixServer::send_to_client(int client_fd, uint32_t type, const uint8_t* payload, size_t len)
{
    unix_message_header hdr{};
    hdr.length = static_cast<uint32_t>(len);
    hdr.type = type;

    // Use writev to send header + payload atomically
    struct iovec iov[2];
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = const_cast<uint8_t*>(payload);
    iov[1].iov_len = len;

    ssize_t written = writev(client_fd, iov, 2);
    if (written < 0) {
        std::cerr << "UnixServer: writev() failed for fd=" << client_fd << ": " << strerror(errno) << std::endl;
        return false;
    }

    return static_cast<size_t>(written) == sizeof(hdr) + len;
}

void UnixServer::broadcast(uint32_t type, const uint8_t* payload, size_t len)
{
    for (const auto& client : clients) {
        send_to_client(client.fd, type, payload, len);
    }
}
