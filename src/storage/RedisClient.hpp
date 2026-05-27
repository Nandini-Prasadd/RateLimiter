#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <queue>
#include <chrono>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using SocketType = SOCKET;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    using SocketType = int;
    #define INVALID_SOCKET_VAL (-1)
    #define SOCKET_ERROR_VAL (-1)
#endif

namespace adaptive_rate_limiter {

class RedisConnection {
public:
    RedisConnection(const std::string& host, int port, int timeout_ms);
    ~RedisConnection();

    bool connect();
    void disconnect();
    bool is_connected() const { return socket_ != INVALID_SOCKET_VAL; }

    // Send a formatted RESP command and parse a 3-element integer array response
    bool execute_lua_eval(const std::string& script_body, 
                          const std::vector<std::string>& keys, 
                          const std::vector<std::string>& args, 
                          std::vector<long long>& out_result);

private:
    std::string host_;
    int port_;
    int timeout_ms_;
    SocketType socket_ = INVALID_SOCKET_VAL;

    bool send_all(const std::string& data);
    bool read_until_crlf(std::string& out_line);
    bool read_bytes(char* buffer, size_t bytes_to_read);
    bool set_socket_timeouts();

    // RESP Parsing helpers
    bool parse_resp_reply(std::vector<long long>& out_result);
};

class RedisClient {
public:
    RedisClient(std::string host, int port, int timeout_ms = 200, size_t pool_size = 10);
    ~RedisClient();

    // Executes the token bucket Lua script.
    // Returns true on success, filling out_result with [allowed, remaining_tokens, retry_after_ms]
    bool execute_token_bucket(const std::string& script, 
                              const std::string& key, 
                              double capacity, 
                              double refill_rate_per_ms, 
                              int cost, 
                              long long now_ms, 
                              std::vector<long long>& out_result);

private:
    std::unique_ptr<RedisConnection> acquire_connection();
    void release_connection(std::unique_ptr<RedisConnection> conn);

    std::string host_;
    int port_;
    int timeout_ms_;
    size_t pool_size_;

    std::queue<std::unique_ptr<RedisConnection>> pool_;
    std::mutex pool_mutex_;

    static void initialize_networking();
    static void cleanup_networking();
    static std::once_flag init_flag_;
};

} // namespace adaptive_rate_limiter
