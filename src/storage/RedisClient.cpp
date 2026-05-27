#include "storage/RedisClient.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
#endif

namespace adaptive_rate_limiter {

std::once_flag RedisClient::init_flag_;

void RedisClient::initialize_networking() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
#endif
}

void RedisClient::cleanup_networking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// ================= RedisConnection Implementation =================

RedisConnection::RedisConnection(const std::string& host, int port, int timeout_ms)
    : host_(host), port_(port), timeout_ms_(timeout_ms) {}

RedisConnection::~RedisConnection() {
    disconnect();
}

bool RedisConnection::connect() {
    disconnect();

    struct addrinfo hints, *result = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return false;
    }

    socket_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socket_ == INVALID_SOCKET_VAL) {
        freeaddrinfo(result);
        return false;
    }

    // Connect with simple blocking socket
    if (::connect(socket_, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR_VAL) {
        disconnect();
        freeaddrinfo(result);
        return false;
    }

    freeaddrinfo(result);

    if (!set_socket_timeouts()) {
        disconnect();
        return false;
    }

    return true;
}

void RedisConnection::disconnect() {
    if (socket_ != INVALID_SOCKET_VAL) {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = INVALID_SOCKET_VAL;
    }
}

bool RedisConnection::set_socket_timeouts() {
#ifdef _WIN32
    DWORD timeout = timeout_ms_;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR_VAL) {
        return false;
    }
    if (setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR_VAL) {
        return false;
    }
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return false;
    }
    if (setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        return false;
    }
#endif
    return true;
}

bool RedisConnection::send_all(const std::string& data) {
    if (!is_connected()) return false;
    size_t total_sent = 0;
    const char* ptr = data.data();
    while (total_sent < data.length()) {
        int sent = send(socket_, ptr + total_sent, static_cast<int>(data.length() - total_sent), 0);
        if (sent <= 0) {
            disconnect(); // connection broken
            return false;
        }
        total_sent += sent;
    }
    return true;
}

bool RedisConnection::read_until_crlf(std::string& out_line) {
    out_line.clear();
    char ch;
    while (true) {
        int bytes = recv(socket_, &ch, 1, 0);
        if (bytes <= 0) {
            disconnect();
            return false;
        }
        if (ch == '\n') {
            if (!out_line.empty() && out_line.back() == '\r') {
                out_line.pop_back(); // Remove \r
            }
            return true;
        }
        out_line.push_back(ch);
    }
}

bool RedisConnection::read_bytes(char* buffer, size_t bytes_to_read) {
    size_t total_read = 0;
    while (total_read < bytes_to_read) {
        int bytes = recv(socket_, buffer + total_read, static_cast<int>(bytes_to_read - total_read), 0);
        if (bytes <= 0) {
            disconnect();
            return false;
        }
        total_read += bytes;
    }
    return true;
}

bool RedisConnection::parse_resp_reply(std::vector<long long>& out_result) {
    std::string line;
    if (!read_until_crlf(line)) return false;

    if (line.empty()) return false;

    char type = line[0];
    if (type == '-') {
        // Redis Error
        std::cerr << "Redis execution error: " << line.substr(1) << std::endl;
        return false;
    }

    if (type == '*') {
        // RESP Array
        int count = std::stoi(line.substr(1));
        if (count < 0) return false;

        out_result.clear();
        out_result.reserve(count);

        for (int i = 0; i < count; ++i) {
            std::string item_line;
            if (!read_until_crlf(item_line)) return false;

            if (item_line.empty()) return false;

            if (item_line[0] == ':') {
                // Integer
                out_result.push_back(std::stoll(item_line.substr(1)));
            } else if (item_line[0] == '$') {
                // Bulk String inside array (could happen for other Lua returns, but our script returns integers)
                int len = std::stoi(item_line.substr(1));
                if (len < 0) {
                    out_result.push_back(0); // represent nil as 0
                } else {
                    std::vector<char> buf(len + 2); // len + \r\n
                    if (!read_bytes(buf.data(), len + 2)) return false;
                    buf[len] = '\0';
                    out_result.push_back(std::stoll(buf.data()));
                }
            } else {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool RedisConnection::execute_lua_eval(const std::string& script_body, 
                                      const std::vector<std::string>& keys, 
                                      const std::vector<std::string>& args, 
                                      std::vector<long long>& out_result) {
    if (!is_connected()) {
        if (!connect()) return false;
    }

    // Construct EVAL command in RESP
    std::string cmd = "*" + std::to_string(3 + keys.size() + args.size()) + "\r\n";
    
    auto append_bulk = [](std::string& dest, const std::string& str) {
        dest += "$" + std::to_string(str.length()) + "\r\n" + str + "\r\n";
    };

    append_bulk(cmd, "EVAL");
    append_bulk(cmd, script_body);
    append_bulk(cmd, std::to_string(keys.size()));

    for (const auto& key : keys) {
        append_bulk(cmd, key);
    }
    for (const auto& arg : args) {
        append_bulk(cmd, arg);
    }

    if (!send_all(cmd)) {
        // Try reconnecting once if the socket was closed silently
        if (connect()) {
            if (!send_all(cmd)) return false;
        } else {
            return false;
        }
    }

    return parse_resp_reply(out_result);
}

// ================= RedisClient Implementation =================

RedisClient::RedisClient(std::string host, int port, int timeout_ms, size_t pool_size)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms), pool_size_(pool_size) {
    std::call_once(init_flag_, initialize_networking);

    // Warm up connection pool
    for (size_t i = 0; i < pool_size_; ++i) {
        auto conn = std::make_unique<RedisConnection>(host_, port_, timeout_ms_);
        conn->connect(); // pre-connect
        pool_.push(std::move(conn));
    }
}

RedisClient::~RedisClient() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
    // Note: We don't cleanup Winsock on shutdown because other instances might be alive,
    // and standard practice is to let the OS handle it, or we could coordinate static refs.
}

std::unique_ptr<RedisConnection> RedisClient::acquire_connection() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (pool_.empty()) {
        // Create an on-demand connection if pool is starved
        return std::make_unique<RedisConnection>(host_, port_, timeout_ms_);
    }
    auto conn = std::move(pool_.front());
    pool_.pop();
    return conn;
}

void RedisClient::release_connection(std::unique_ptr<RedisConnection> conn) {
    if (!conn) return;
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (pool_.size() < pool_size_) {
        pool_.push(std::move(conn));
    }
    // If pool is full, let conn go out of scope to disconnect and delete
}

bool RedisClient::execute_token_bucket(const std::string& script, 
                                      const std::string& key, 
                                      double capacity, 
                                      double refill_rate_per_ms, 
                                      int cost, 
                                      long long now_ms, 
                                      std::vector<long long>& out_result) {
    auto conn = acquire_connection();
    if (!conn) return false;

    std::vector<std::string> keys = { key };
    std::vector<std::string> args = {
        std::to_string(capacity),
        std::to_string(refill_rate_per_ms),
        std::to_string(cost),
        std::to_string(now_ms)
    };

    bool success = conn->execute_lua_eval(script, keys, args, out_result);
    release_connection(std::move(conn));
    return success;
}

} // namespace adaptive_rate_limiter
