# Redis Benchmark: FIFO vs LRU Page Replacement Policies

This document compares the performance of Redis under two different page replacement algorithms in our KVM environment:
1. FIFO (First-In-First-Out) - The default policy
2. Approximate LRU (Least Recently Used) - Our implementation

## Benchmark Configuration
- 1 million requests (`-n 1000000`)
- 100,000 distinct keys (`-r 100000`) 
- Pipeline of 16 commands (`-P 16`)
- Quiet output format (`-q`)

## Benchmark Results

### FIFO (Default)
```bash
ubuntu@ubuntu:~$ redis-cli flushall && redis-benchmark -n 1000000 -r 100000 -P 16 -q
PING_INLINE: 488519.81 requests per second, p50=1.391 msec                    
PING_MBULK: 343878.97 requests per second, p50=1.167 msec                    
SET: 286450.88 requests per second, p50=2.271 msec                    
GET: 244857.97 requests per second, p50=2.319 msec                    
INCR: 314465.41 requests per second, p50=2.095 msec                    
LPUSH: 378357.94 requests per second, p50=1.823 msec                    
RPUSH: 392618.78 requests per second, p50=1.743 msec                    
LPOP: 360490.28 requests per second, p50=1.903 msec                    
RPOP: 358937.53 requests per second, p50=1.871 msec                    
SADD: 324044.06 requests per second, p50=1.855 msec                    
HSET: 286123.03 requests per second, p50=2.335 msec                    
SPOP: 434216.25 requests per second, p50=1.447 msec                    
ZADD: 158227.84 requests per second, p50=4.255 msec                    
ZPOPMIN: 430848.75 requests per second, p50=1.439 msec                    
LPUSH (needed to benchmark LRANGE): 362713.09 requests per second, p50=1.815 msec                    
LRANGE_100 (first 100 elements): 73324.54 requests per second, p50=4.991 msec                    
LRANGE_300 (first 300 elements): 18265.17 requests per second, p50=22.911 msec                    
LRANGE_500 (first 500 elements): 12205.99 requests per second, p50=21.583 msec                    
LRANGE_600 (first 600 elements): 9559.50 requests per second, p50=36.511 msec                     
MSET (10 keys): 92575.45 requests per second, p50=7.647 msec                     
XADD: 278318.97 requests per second, p50=2.591 msec
```

### LRU (Ours)
```bash
ubuntu@ubuntu:~$ redis-cli flushall && redis-benchmark -n 1000000 -r 100000 -P 16 -q
PING_INLINE: 313676.28 requests per second, p50=1.655 msec                    
PING_MBULK: 470588.25 requests per second, p50=1.255 msec                    
SET: 330469.25 requests per second, p50=2.079 msec                    
GET: 362187.62 requests per second, p50=1.823 msec                    
INCR: 316555.88 requests per second, p50=2.143 msec                    
LPUSH: 332005.31 requests per second, p50=1.895 msec                    
RPUSH: 362056.47 requests per second, p50=1.799 msec                    
LPOP: 358166.19 requests per second, p50=1.959 msec                    
RPOP: 346860.91 requests per second, p50=1.919 msec                    
SADD: 345423.16 requests per second, p50=1.927 msec                    
HSET: 250438.27 requests per second, p50=2.655 msec                    
SPOP: 433839.47 requests per second, p50=1.487 msec                    
ZADD: 144696.86 requests per second, p50=4.591 msec                    
ZPOPMIN: 427167.88 requests per second, p50=1.447 msec                    
LPUSH (needed to benchmark LRANGE): 299132.53 requests per second, p50=1.975 msec                    
LRANGE_100 (first 100 elements): 67640.69 requests per second, p50=5.511 msec                    
LRANGE_300 (first 300 elements): 16547.80 requests per second, p50=25.535 msec                    
LRANGE_500 (first 500 elements): 12220.75 requests per second, p50=20.687 msec                    
LRANGE_600 (first 600 elements): 9668.28 requests per second, p50=31.823 msec                     
MSET (10 keys): 89485.46 requests per second, p50=7.855 msec                     
XADD: 255232.27 requests per second, p50=2.695 msec
```

## Performance Analysis

### Key Observations

| Operation   | FIFO Performance | LRU Performance | % Difference | Notes                                          |
| ----------- | ---------------- | --------------- | ------------ | ---------------------------------------------- |
| PING_INLINE | 488,520 req/s    | 313,676 req/s   | -35.8%       | Simple operation shows FIFO overhead advantage |
| PING_MBULK  | 343,879 req/s    | 470,588 req/s   | +36.8%       | LRU performs better for bulk ping              |
| GET         | 244,858 req/s    | 362,188 req/s   | +47.9%       | LRU shows significant advantage for reads      |
| SET         | 286,451 req/s    | 330,469 req/s   | +15.4%       | LRU performs better for writes                 |
| ZADD        | 158,228 req/s    | 144,697 req/s   | -8.6%        | Complex operations show FIFO advantage         |
| LRANGE_300  | 18,265 req/s     | 16,548 req/s    | -9.4%        | Bulk retrieval slightly favors FIFO            |
| LRANGE_600  | 9,560 req/s      | 9,668 req/s     | +1.1%        | Larger bulk retrieval slightly favors LRU      |

### Performance Characteristics

1. **Command Complexity Impact**
   * **Simple Operations**: FIFO excels at basic operations like PING_INLINE due to minimal overhead.
   * **Data Access Operations**: LRU demonstrates notably better performance for GET operations (+47.9%), suggesting its page management benefits read-heavy workloads.
   * **Write Operations**: LRU also performs better on SET operations (+15.4%), indicating efficient page allocation for writes.
   * **List Operations**: FIFO shows advantages for LPUSH and RPUSH operations, while results are mixed for different LRANGE sizes.

2. **Latency Analysis**
   * **Read Latency**: GET operations show lower latency with LRU (1.823ms vs 2.319ms), a 21.4% improvement.
   * **Complex Operations**: While LRANGE_300 shows higher latency in LRU, notably LRANGE_500 and LRANGE_600 perform better under LRU, with LRANGE_600 showing significant latency improvement (31.823ms vs 36.511ms).
   * **Overall P50**: Most operations show comparable latency profiles between the two policies, with differences typically under 0.5ms.

3. **Memory Management Implications**
   * LRU's overhead affects throughput for simple operations but its intelligent page replacement appears to benefit data retrieval patterns.
   * FIFO's simplicity provides an advantage for high-throughput, low-complexity operations where memory access patterns are less important.

## Conclusions

* **FIFO Benefits**: Higher throughput for simple operations and complex sorted set operations. Better choice for write-heavy workloads with simple access patterns.

* **LRU Benefits**: Superior performance for data retrieval operations and better overall latency for read operations. Preferred for read-heavy workloads with localized access patterns.

* **Production Recommendation**: For general Redis usage in our environment, the performance profile suggests LRU would be advantageous for applications with read-heavy workloads or those benefiting from locality of reference, while FIFO might be better for high-throughput, write-intensive applications.
