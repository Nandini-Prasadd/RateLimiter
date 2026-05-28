#include <iostream>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#include "storage/RedisLimiter.hpp"
#include "storage/RedisPool.hpp"


std::queue<int> task_queue;
std::mutex queue_mutex;
std::condition_variable cv;

int total_requests = 0;
int allowed_requests = 0;
int blocked_requests = 0;
std::mutex metrics_mutex;

std::unordered_map<std::string, int> rateLimits = {
    {"userA", 100},
    {"userB", 10},
    {"anonymous", 5}
};


void handleClient(int client_socket, RedisPool& pool) {
    char buffer[1024] = {0};
    read(client_socket, buffer, 1024);

    std::string request(buffer);

    if (request.find("GET /metrics") != std::string::npos) {
        std::string metrics;

        {
            std::lock_guard<std::mutex> lock(metrics_mutex);
            metrics =
                "HTTP/1.1 200 OK\r\n\r\n"
                "total_requests " + std::to_string(total_requests) + "\n" +
                "allowed_requests " + std::to_string(allowed_requests) + "\n" +
                "blocked_requests " + std::to_string(blocked_requests) + "\n";
        }

        send(client_socket, metrics.c_str(), metrics.size(), 0);
        close(client_socket);
        return;
    }

    std::string user = "anonymous";

    size_t pos = request.find("x-api-key:");
    if (pos != std::string::npos) {
        size_t end = request.find("\r\n", pos);
        user = request.substr(pos + 11, end - (pos + 11));
    }

    auto it = rateLimits.find(user);
    int limit = (it != rateLimits.end()) ? it->second : 5;

    thread_local RedisLimiter limiter(pool);

    int remaining, reset;
    bool allowed = limiter.allowRequest(user, limit, remaining, reset);

    {
        std::lock_guard<std::mutex> lock(metrics_mutex);
        total_requests++;

        if (allowed) allowed_requests++;
        else blocked_requests++;

        if (total_requests % 100 == 0) {
            std::cout << "\n=== METRICS ===\n";
            std::cout << "Total: " << total_requests << "\n";
            std::cout << "Allowed: " << allowed_requests << "\n";
            std::cout << "Blocked: " << blocked_requests << "\n";
            std::cout << "================\n";
        }
    }

    // std::cout << "[LOG] user=" << user
    //           << " allowed=" << allowed
    //           << " remaining=" << remaining
    //           << " reset=" << reset
    //           << std::endl;

    std::string response;

    if (allowed) {
        response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "X-RateLimit-Limit: " + std::to_string(limit) + "\r\n"
            "X-RateLimit-Remaining: " + std::to_string(remaining) + "\r\n"
            "X-RateLimit-Reset: " + std::to_string(reset) + "\r\n\r\n"
            "Allowed\n";
    } else {
        response =
            "HTTP/1.1 429 Too Many Requests\r\n"
            "Content-Type: text/plain\r\n"
            "X-RateLimit-Limit: " + std::to_string(limit) + "\r\n"
            "X-RateLimit-Remaining: 0\r\n"
            "X-RateLimit-Reset: " + std::to_string(reset) + "\r\n\r\n"
            "Blocked\n";
    }

    send(client_socket, response.c_str(), response.size(), 0);
    close(client_socket);
}


void worker(RedisPool& pool) {
    while (true) {
        int client_socket;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !task_queue.empty(); });

            client_socket = task_queue.front();
            task_queue.pop();
        }

        handleClient(client_socket, pool);
    }
}


int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 1000);

    std::cout << "Optimized thread pool server running on port 8080...\n";

    RedisPool pool(50);

    int THREAD_COUNT = 50;
    std::vector<std::thread> workers;

    for (int i = 0; i < THREAD_COUNT; i++) {
        workers.emplace_back(worker, std::ref(pool));
    }

    while (true) {
        int client_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            task_queue.push(client_socket);
        }

        cv.notify_one();
    }

    return 0;
}
