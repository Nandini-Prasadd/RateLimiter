#pragma once
#include <hiredis/hiredis.h>
#include <queue>
#include <mutex>
#include <condition_variable>

class RedisPool {
private:
    std::queue<redisContext*> pool;
    std::mutex mtx;
    std::condition_variable cv;

public:
    RedisPool(int size = 10) {
        for (int i = 0; i < size; i++) {
            redisContext* ctx = redisConnect("127.0.0.1", 6379);

            if (!ctx || ctx->err) {
                if (ctx) {
                    printf("Redis error: %s\n", ctx->errstr);
                } else {
                    printf("Redis allocation error\n");
                }
            }

            pool.push(ctx);
        }
    }

    redisContext* getConnection() {
        std::unique_lock<std::mutex> lock(mtx);

        cv.wait(lock, [&]() { return !pool.empty(); });

        redisContext* ctx = pool.front();
        pool.pop();

        return ctx;
    }

    void returnConnection(redisContext* ctx) {
        std::lock_guard<std::mutex> lock(mtx);
        pool.push(ctx);
        cv.notify_one();
    }
};
