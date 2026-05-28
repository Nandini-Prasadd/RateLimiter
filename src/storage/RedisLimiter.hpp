#pragma once
#include <string>
#include <hiredis/hiredis.h>
#include "RedisPool.hpp"

class RedisLimiter {
private:
    RedisPool& pool;

    const char* script =
        "local key = KEYS[1] "
        "local limit = tonumber(ARGV[1]) "
        "local count = redis.call('INCR', key) "
        "if count == 1 then redis.call('EXPIRE', key, 1) end "
        "local ttl = redis.call('TTL', key) "
        "if count > limit then "
        "  return {0, limit - count, ttl} "
        "else "
        "  return {1, limit - count, ttl} "
        "end";

public:
    RedisLimiter(RedisPool& p) : pool(p) {}

    bool allowRequest(const std::string& user, int limit, int& remaining, int& reset) {

        redisContext* ctx = pool.getConnection();

        redisReply* reply = (redisReply*)redisCommand(
            ctx,
            "EVAL %s 1 %s %d",
            script,
            user.c_str(),
            limit
        );

        bool allowed = reply->element[0]->integer;
        remaining = reply->element[1]->integer;
        reset = reply->element[2]->integer;

        if (reset < 0) reset = 1;

        freeReplyObject(reply);

        pool.returnConnection(ctx);

        return allowed;
    }
};
