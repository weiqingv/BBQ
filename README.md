# BBQ: A Block-based Bounded Queue for Exchanging Data and Profiling
This repositry provides a concurrent lock-free queue that supports multiple producers and multiple consumers. The C++ implementaion is originated from the idea of the paper \[1\]. We use CAS and while loops instead of MAX atomic operation in x86 platforms.
## Requirements
The project is based on the C++ 17 standards and leverages several relevent features, e.g. \<vairant\>.  
The minimum version of CMake: 3.21.  
The benchmark script requires boost v1.82.0.
## Usage
The implementation is head-only and can be integrated in other projects seamlessly.
```cpp
// simply includes bbq.h
#include "bbq.h"
using namespace bbq;

int main() {
  // define a queue with block num of 5, block size of 10, and enqueue policy of retry-new.
  BlockBoundedQueue<unsigned, 5, 10> queue(Policy::RETRY_NEW);

  // enqueue until success
  // failure reasons see QueueState and QueueStatus in bbq.h 
  while (queue.enqueue(s).index() != OKAY);

  // dequeue and unpack data
  auto res = queue.dequeue();
  auto data = std::get_if<OK<unsigned>>(&res)->data;

  return 0;
}
```
## Benchmark
We compared the throughput of BBQ with various blocking or lock-free queue implementations\[2-\5], boost lockfree queues, and native synchronized queues.
The test script comes from \[6\].  
*Environment*:   
Ubuntu 18.04 \(Linux 5.4.0 kernel\) with 12 CPU cores under x86-64 arch.  
*Results*:   
In the case of a large number of producers and consumers, the throughput achieved by our BBQ implementation reaches SOTA.  
More detail are descibed in bench_test.txt.

## Reference
\[1\] https://www.usenix.org/conference/atc22/presentation/wang-jiawei   
\[2\] https://github.com/gongyiling/cpp_lecture/tree/main/lockfree  
\[3\] https://github.com/cameron314/concurrentqueue  
\[4\] https://github.com/mstump/queues  
\[5\] https://github.com/rigtorp/MPMCQueue.git  
\[6\] https://gist.github.com/TurpentineDistillery/benchmarks.cpp
