import urllib.request
import urllib.parse
import threading
import time
import argparse
import sys
import json
from collections import Counter

# Thread-safe counter for statistics
class BenchmarkStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.status_codes = Counter()
        self.latencies = []
        self.total_requests = 0

    def record(self, status, latency):
        with self.lock:
            self.status_codes[status] += 1
            self.latencies.append(latency)
            self.total_requests += 1

    def get_summary(self):
        with self.lock:
            if not self.latencies:
                return {}
            sorted_lat = sorted(self.latencies)
            p50 = sorted_lat[int(len(sorted_lat) * 0.50)]
            p95 = sorted_lat[int(len(sorted_lat) * 0.95)]
            p99 = sorted_lat[int(len(sorted_lat) * 0.99)]
            avg = sum(self.latencies) / len(self.latencies)
            return {
                "total": self.total_requests,
                "codes": dict(self.status_codes),
                "avg_ms": avg * 1000.0,
                "p50_ms": p50 * 1000.0,
                "p95_ms": p95 * 1000.0,
                "p99_ms": p99 * 1000.0
            }

def send_request(url, plan, cost, stats):
    req = urllib.request.Request(url)
    req.add_header("X-Test-Plan", plan)
    req.add_header("X-API-Key", f"benchmark_{plan}_key")
    
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            status = response.status
            response.read() # consume body
    except urllib.error.HTTPError as e:
        status = e.code
        try:
            e.read()
        except:
            pass
    except Exception as e:
        status = 0 # connection error / timeout
    
    end = time.perf_counter()
    stats.record(status, end - start)

def worker_loop(url, plan, cost, rate_limit_rps, duration, stop_event, stats):
    start_time = time.time()
    
    # If a specific rate is requested, handle pacing
    if rate_limit_rps > 0:
        interval = 1.0 / rate_limit_rps
        while not stop_event.is_set() and (time.time() - start_time < duration):
            req_start = time.perf_counter()
            send_request(url, plan, cost, stats)
            elapsed = time.perf_counter() - req_start
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)
    else:
        # Full throttle loop
        while not stop_event.is_set() and (time.time() - start_time < duration):
            send_request(url, plan, cost, stats)

def fetch_metrics(metrics_url):
    try:
        with urllib.request.urlopen(metrics_url, timeout=2) as res:
            lines = res.read().decode('utf-8').split('\n')
            
            multipliers = {}
            p95 = "0.0"
            storage_errs = "0"
            active_reqs = "0"

            for line in lines:
                if line.startswith("rate_limiter_adaptive_multiplier"):
                    # e.g., rate_limiter_adaptive_multiplier{plan="free"} 1.000
                    parts = line.split(" ")
                    val = parts[-1]
                    plan = "free"
                    if "premium" in parts[0]: plan = "premium"
                    elif "pro" in parts[0]: plan = "pro"
                    multipliers[plan] = val
                elif line.startswith("rate_limiter_backend_latency_p95_ms"):
                    p95 = line.split(" ")[-1]
                elif line.startswith("rate_limiter_storage_errors_total"):
                    storage_errs = line.split(" ")[-1]
                elif line.startswith("rate_limiter_active_requests"):
                    active_reqs = line.split(" ")[-1]

            return multipliers, p95, storage_errs, active_reqs
    except Exception as e:
        return None

def main():
    parser = argparse.ArgumentParser(description="Adaptive Rate Limiter Benchmark Harness")
    parser.add_argument("--url", default="http://localhost:8080/proxy/work", help="Target Proxy endpoint")
    parser.add_argument("--metrics-url", default="http://localhost:8080/metrics", help="Proxy metrics endpoint")
    parser.add_argument("--plan", default="free", choices=["free", "pro", "premium"], help="User Plan level to test")
    parser.add_argument("--cost", type=int, default=1, help="Request cost size")
    parser.add_argument("--threads", type=int, default=10, help="Number of concurrent execution threads")
    parser.add_argument("--rps-per-thread", type=int, default=-1, help="Target RPS per thread (-1 for maximum speed)")
    parser.add_argument("--duration", type=int, default=15, help="Benchmark duration in seconds")
    parser.add_argument("--work-latency", type=int, default=20, help="Simulated work latency inside backend")
    parser.add_argument("--work-error", type=float, default=0.0, help="Simulated backend error rate (0.0 to 1.0)")
    args = parser.parse_args()

    # Append query params to url to tell Backend Simulator what delay to simulate
    parsed_url = urllib.parse.urlparse(args.url)
    query = urllib.parse.parse_qs(parsed_url.query)
    query['latency_ms'] = str(args.work_latency)
    query['error_rate'] = str(args.work_error)
    new_query = urllib.parse.urlencode(query)
    target_url = urllib.parse.urlunparse((
        parsed_url.scheme, parsed_url.netloc, parsed_url.path,
        parsed_url.params, new_query, parsed_url.fragment
    ))

    print("=" * 60)
    print("        Adaptive API Rate Limiter - Benchmark Harness")
    print("=" * 60)
    print(f"Target URL:     {target_url}")
    print(f"Plan (Priority): {args.plan.upper()}")
    print(f"Threads:        {args.threads}")
    print(f"Pacing:         {'Unlimited' if args.rps_per_thread <= 0 else f'{args.rps_per_thread} RPS/thread'}")
    print(f"Duration:       {args.duration} seconds")
    print(f"Work Sim:       Latency={args.work_latency}ms, ErrorRate={args.work_error*100}%")
    print("-" * 60)

    stats = BenchmarkStats()
    stop_event = threading.Event()

    threads = []
    for _ in range(args.threads):
        t = threading.Thread(
            target=worker_loop,
            args=(target_url, args.plan, args.cost, args.rps_per_thread, args.duration, stop_event, stats)
        )
        threads.append(t)

    # Start monitor thread to watch metrics live
    def monitor_loop():
        start = time.time()
        print(f"{'Time':<6} | {'Reqs':<6} | {'RPS':<6} | {'200s':<5} | {'429s':<5} | {'503s':<5} | {'p95_ms':<7} | {'Multiplier':<10}")
        print("-" * 75)
        last_req = 0
        while not stop_event.is_set() and (time.time() - start < args.duration):
            time.sleep(2.0)
            elapsed = time.time() - start
            met = fetch_metrics(args.metrics_url)
            summary = stats.get_summary()
            
            if summary:
                tot = summary["total"]
                delta_reqs = tot - last_req
                last_req = tot
                rps = delta_reqs / 2.0
                codes = summary["codes"]
                c200 = codes.get(200, 0)
                c429 = codes.get(429, 0)
                c503 = codes.get(503, 0)
            else:
                rps = 0
                c200 = c429 = c503 = 0

            if met:
                mults, p95, st_errs, active = met
                mult = mults.get(args.plan, "1.000")
            else:
                mult = "N/A"
                p95 = "N/A"

            print(f"{elapsed:4.1f}s  | {stats.total_requests:6} | {rps:6.1f} | {c200:5} | {c429:5} | {c503:5} | {p95:<7} | {mult:<10}")

    monitor_thread = threading.Thread(target=monitor_loop)
    
    # Fire threads
    for t in threads:
        t.start()
    monitor_thread.start()

    # Wait for duration
    time.sleep(args.duration)
    stop_event.set()

    # Join threads
    for t in threads:
        t.join()
    monitor_thread.join()

    print("=" * 60)
    print("                  BENCHMARK COMPLETE")
    print("=" * 60)
    
    summary = stats.get_summary()
    if not summary:
        print("No requests completed successfully.")
        return

    print(f"Total Requests Processed: {summary['total']}")
    print(f"HTTP Status Codes Breakdown:")
    for code, count in summary['codes'].items():
        desc = "Success" if code == 200 else ("Rate Limited" if code == 429 else ("Load Shedded" if code == 503 else "Error"))
        print(f"  - {code}: {count} ({desc})")
    
    print("-" * 60)
    print(f"Client Measured Latency:")
    print(f"  - Average: {summary['avg_ms']:.2f} ms")
    print(f"  - p50:     {summary['p50_ms']:.2f} ms")
    print(f"  - p95:     {summary['p95_ms']:.2f} ms")
    print(f"  - p99:     {summary['p99_ms']:.2f} ms")
    
    met = fetch_metrics(args.metrics_url)
    if met:
        mults, p95, st_errs, active = met
        print("-" * 60)
        print("Rate Limiter Final Internal Metrics:")
        print(f"  - p95 Latency Window (Proxy):  {p95} ms")
        print(f"  - Storage Errors Counter:     {st_errs}")
        print(f"  - Current Multiplier:         {mults.get(args.plan)}")
        print(f"  - Active Connections Count:   {active}")
    print("=" * 60)

if __name__ == "__main__":
    main()
