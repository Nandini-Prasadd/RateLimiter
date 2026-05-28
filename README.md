# Adaptive Distributed Rate Limiter (C++ + Redis)

## Overview

This project implements a **high-performance distributed rate limiter** using **C++**, **Redis**, and **Lua scripting**. It supports **per-API key rate limiting**, efficient concurrency using a **thread pool**, and achieves **~10k requests/sec** under load.

Designed to simulate real-world backend infrastructure used in production systems.

---

## Features

* **API Key-based Rate Limiting**
* **High Throughput (~10k req/sec locally)**
* **Redis + Lua (Atomic operations)**
* **Thread Pool (Concurrent request handling)**
* **Redis Connection Pool**
* **Metrics Endpoint (`/metrics`)**
* **Rate Limit Headers (X-RateLimit-*)**
* **Load Tested using ApacheBench**

---

## Architecture

```
Client → C++ Server → Thread Pool → Redis (Lua Script)
```

### Flow:

1. Request arrives with optional `x-api-key`
2. Worker thread picks request
3. Redis Lua script:

   * Increments request count
   * Sets expiry (1 sec window)
   * Returns allowed / remaining / TTL
4. Server responds with:

   * Status (Allowed / Blocked)
   * Rate limit headers

---

## Rate Limiting Strategy

Uses **Fixed Window Counter** with Redis:

* `INCR key`
* `EXPIRE key 1`
* Lua ensures **atomic execution**

---

## API Key Limits

| API Key   | Limit (req/sec) |
| --------- | --------------- |
| userA     | 100             |
| userB     | 10              |
| anonymous | 5               |

---

## Endpoints

### `/`

Main endpoint

Headers supported:

```
x-api-key: userA
```

---

### `/metrics`

Returns system metrics:

```
total_requests
allowed_requests
blocked_requests
```

---

## Sample Response

```
HTTP/1.1 200 OK
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 99
X-RateLimit-Reset: 1

Allowed
```

---

## Load Testing

Tested using ApacheBench:

```
ab -n 5000 -c 100 http://localhost:8080/
```
With API key:
```
curl -H "x-api-key: userA" http://localhost:8080
```

### Results

| API Key   | Requests/sec |
| --------- | ------------ |
| anonymous | ~9400 req/s  |
| userA     | ~10400 req/s |
| userB     | ~10700 req/s |

* Non-2xx responses expected due to rate limiting
* Zero failed requests

---

## Performance Optimizations

* `thread_local` RedisLimiter (avoids object creation)
* Redis connection pooling
* Disabled logging during benchmarking
* Efficient hashmap lookup for API keys

---

## Tech Stack

* **C++ (Sockets, Threads)**
* **Redis**
* **Lua (Atomic scripts)**
* **hiredis**
* **ApacheBench**

---

## How to Run

### 1. Start Redis

```
redis-server
```

### 2. Build Project

```
mkdir build
cd build
cmake ..
make
```

### 3. Run Server

```
./server
```

---

## Key Learnings

* Designing scalable rate limiters
* Handling concurrency with thread pools
* Using Redis for distributed systems
* Performance tuning and bottleneck analysis

---

## Future Improvements

* Token Bucket / Sliding Window algorithms
* Distributed deployment (multi-node)
* Prometheus + Grafana integration
* Async networking (epoll)

---

## Author

Nandini Prasad
