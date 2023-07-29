// adopted from https://gist.github.com/TurpentineDistillery/benchmarks.cpp

#include <queue>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <chrono>
#include <cassert>
#include <iostream>
#include <iomanip>

#include <sys/utsname.h>

// outside of diagnostic push because occur in blockingconcurrentqueue's instantiations
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wextra"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"

#ifdef WITH_BOOST_FIBER
#include <boost/fiber/bounded_channel.hpp>
#endif

#include <boost/thread/sync_bounded_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include "concurrentqueue/blockingconcurrentqueue.h" // moodycamel
#include "queues/include/mpmc-bounded-queue.h" // https://github.com/mstump/queues
// https://int08h.com/post/ode-to-a-vyukov-queue/
#include "lockfree_queue.h"  // https://github.com/gongyiling/cpp_lecture/tree/main/lockfree
#include "../bbq.h"


// required by MPMCQueue.h; missing in earlier stdlib
//namespace std {
//    inline void *align(std::size_t alignment, std::size_t size,
//                       void *&ptr, std::size_t &space) noexcept {
//        std::uintptr_t pn = reinterpret_cast< std::uintptr_t >( ptr );
//        std::uintptr_t aligned = (pn + alignment - 1) & -alignment;
//        std::size_t padding = aligned - pn;
//        if (space < size + padding) return nullptr;
//        space -= padding;
//        return ptr = reinterpret_cast< void * >( aligned );
//    }
//}

#include "MPMCQueue.h" //https://github.com/rigtorp/MPMCQueue.git


#pragma GCC diagnostic pop

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////

// synchronized-queues capacity
// NB: mpmc-bounded-queues requires power of two capacity
//static const size_t g_capacity = 4096;
static const size_t g_capacity = 16384;

// will pump this many elements through the queues
//static const size_t g_num_elements = g_capacity * std::thread::hardware_concurrency();
static const size_t g_num_elements = 16384 * 12;

namespace mt {

/////////////////////////////////////////////////////////////////////////////
    struct timer {
        /// returns the time elapsed since timer's instantiation, in seconds.
        /// To reset: `my_timer = mt::timer{}`.
        operator double() const {
            return (double) std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock_t::now() - start_timepoint).count() * 1e-9;
        }

    private:
        using clock_t = std::chrono::steady_clock;
        clock_t::time_point start_timepoint = clock_t::now();
    };


/////////////////////////////////////////////////////////////////////////////
/// \brief Can be used as alternative to std::mutex.
/// It is typically faster than std::mutex, yet does not aggressively max-out the CPUs.
/// NB: may cause thread starvation in some scenarios.
    template<uint64_t SleepMicrosec = 50>
    class atomic_lock {
        std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
    public:

        atomic_lock() = default;

        // non-copyable and non-movable, as with std::mutex

        atomic_lock(const atomic_lock &) = delete;

        atomic_lock &operator=(const atomic_lock &) = delete;

        atomic_lock(atomic_lock &&) = delete;

        atomic_lock &operator=(atomic_lock &&) = delete;

        /////////////////////////////////////////////////////////////////////////
        bool try_lock() noexcept {
            return !m_flag.test_and_set(std::memory_order_acquire);
        }

        void lock() noexcept {
            while (m_flag.test_and_set(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                        std::chrono::microseconds(SleepMicrosec));
            }
        }

        void unlock() noexcept {
            m_flag.clear(std::memory_order_release);
        }
    };


/////////////////////////////////////////////////////////////////////////////
/// A minimalistic blocking, optionally-bounded synchronized-queues.
/// For testing/comparison/benchmarking. not for production.
/// Only requires MoveAssigneable from value_type
    template<typename T, class BasicLockable = std::mutex /*or mt::atomic_lock*/>
    class naive_synchronized_queue {
    public:
        using value_type = T;

        naive_synchronized_queue(size_t capacity_ = size_t(-1))
                : m_capacity{capacity_ == 0 ? 1 : capacity_} {}

        /////////////////////////////////////////////////////////////////////////
        void push(value_type val) {
            // NB: guards are to prevent thread starvation in
            // MPSC and SPMC scenarios when used with atomic_lock
            guard_t guard{m_push_mutex};
            lock_t lock{m_mutex};

            m_can_push.wait(
                    lock, [this] { return m_queue.size() < m_capacity; });

            m_queue.push(std::move(val));

            lock.unlock();
            m_can_pop.notify_one();
        }

        /////////////////////////////////////////////////////////////////////////
        value_type pop() {
            guard_t guard{m_pop_mutex};
            lock_t lock{m_mutex};

            m_can_pop.wait(
                    lock, [this] { return !m_queue.empty(); });

            value_type ret = std::move(m_queue.front());
            m_queue.pop();

            lock.unlock();
            m_can_push.notify_one();
            return ret;
        }

        /////////////////////////////////////////////////////////////////////////
    private:
        using lock_t = std::unique_lock<BasicLockable>;
        using guard_t = std::lock_guard<BasicLockable>;
        using queue_t = std::queue<value_type>;

        using condvar_t = typename std::conditional<
                std::is_same<BasicLockable, std::mutex>::value,
                std::condition_variable,
                std::condition_variable_any>::type;

        size_t m_capacity;
        queue_t m_queue;
        BasicLockable m_mutex;
        BasicLockable m_push_mutex;
        BasicLockable m_pop_mutex;
        condvar_t m_can_push;
        condvar_t m_can_pop;
    }; // naive_synchronized_queue
    static void min_sleep() {
        std::this_thread::sleep_for(
                std::chrono::nanoseconds(1));
        // NB: sleep a minimal amount to avoid hogging the CPU.
        // (the actual time is greater than 1ns; depends on the CPU scheduler)
    }

} // namespace mt

/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// Normalizing interface to the various synchronized-queues implementations:
//
//
// template<typename T, typename Queue>
// void push(T value); // blocking
//
// template<typename T, typename Queue>
// T pop(); // blocking
//
// Non-blocking calls are wrapped into a busy-wait loops
// (e.g. boost::lockfree below)

template<typename T, typename Queue>
auto push(Queue &q, T value) -> decltype((void) q.push(T{})) {
    q.push(std::move(value));
}

template<typename T>
void push(boost::lockfree::queue <T> &q, T value) {
    // bounded_push blocks if the queues is at capacity
    while (!q.bounded_push(std::move(value))) { mt::min_sleep(); }
}

template<typename T>
void push(moodycamel::BlockingConcurrentQueue<T> &q, T value) {
#if 1
    while (q.size_approx() > g_capacity) {
        mt::min_sleep();
    }
    // NB: insertion may violate the capacity bound,
    // but there's no implicit bounding mechanism
    // in BlockingConcurrentQueue as far as I can tell.
    // We're keeping it "manually" approximately within capacity.
#else
    // Even if we don't cap the capacity, this does not
    // affect the results of the benchmarks.
#endif
    q.enqueue(std::move(value));
}

template<typename T>
void push(moodycamel::ConcurrentQueue<T> &q, T value) {
    while (q.size_approx() > g_capacity) {
        mt::min_sleep(); // see comments in the overload above
    }
    q.enqueue(std::move(value));
}

template<typename T>
void push(LockFreeQueue<T> &q, T value) {
    while (!q.enqueue(value));
}

template<typename T>
void push(mpmc_bounded_queue_t<T> &q, T value) {
    while (!q.enqueue(value)) {
        mt::min_sleep();
    }
}

template<typename T>
void push(bbq::BlockBoundedQueue<T> &q, T value) {
//    while (q.enqueue(value).index() != bbq::OKAY) {
//        mt::min_sleep();
//    }
    while (!q.push(value)) {
        mt::min_sleep();
    }
}

/////////////////////////////////////////////////////////////////////////////

template<typename T, typename Queue>
auto pop(Queue &q) -> decltype(T{q.pop()}) {
    return q.pop();
}

template<typename T, typename Queue>
auto pop(Queue &q) -> decltype((void) q.pop(std::declval<T &>()), T{}) {
    T item{};
    q.pop(item);
    return item;
}

template<typename T>
T pop(boost::sync_bounded_queue <T> &q) {
    return q.pull_front();
}

#ifdef WITH_BOOST_FIBER
template<typename T>
T pop(boost::fibers::bounded_channel<T>& q)
{
    return q.value_pop();
}
#endif

template<typename T>
T pop(boost::lockfree::queue <T> &q) {
    T elem{};
    while (!q.pop(elem)) {
        mt::min_sleep();
    }
    return elem;
}

template<typename T>
T pop(moodycamel::ConcurrentQueue<T> &q) {
    T item{};
    while (!q.try_dequeue(item)) {
        mt::min_sleep();
    }
    return item;
}

template<typename T>
T pop(moodycamel::BlockingConcurrentQueue<T> &q) {
    T item{};
    q.wait_dequeue(item);
    return item;
}

template<typename T>
T pop(LockFreeQueue<T> &q) {
    T item{};
    while (!q.dequeue(item)) {
        mt::min_sleep();
    }
    return item;
}

template<typename T>
T pop(mpmc_bounded_queue_t<T> &q) {
    T item{};
    while (!q.dequeue(item)) {
        mt::min_sleep();
    }
    return item;
}

template<typename T>
T pop(bbq::BlockBoundedQueue<T> &q) {
//    auto stat = q.dequeue();
//    while (stat.index() != bbq::OKAY) {
//        mt::min_sleep();
//        stat = q.dequeue();
//    }
//    return std::get_if<bbq::OK<T>>(&stat)->data;
    T item{};
    while (!q.pop(item)) {
        mt::min_sleep();
    }
    return item;
}

/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////

/// Pump `num_elems` through the queues using specified number of pushing and popping threads
/// @return throughput (items pumped through the queues per second)
template<typename T, class Queue>
double get_throughput(
        Queue &queue,
        size_t num_pushers,
        size_t num_poppers,
        size_t num_elems) {
    // verify that num_pushers and num_poppers both divide num_elems
    assert(num_elems / num_pushers * num_pushers == num_elems);
    assert(num_elems / num_poppers * num_poppers == num_elems);

    std::vector<std::future<long>> pushers{num_pushers};
    std::vector<std::future<long>> poppers{num_poppers};

    mt::timer timer;

    // launch pushing tasks
    for (auto &p: pushers) {
        p = std::async(std::launch::async, [&] {
            long total = 0;
            for (size_t j = 0; j < num_elems / num_pushers; j++) {
                push<T>(queue, 1);
                total++;
            }
            return total;
        });
    }

    // launch popping tasks
    for (auto &p: poppers) {
        p = std::async(std::launch::async, [&] {
            long total = 0;
            for (size_t j = 0; j < num_elems / num_poppers; j++) {
                total += pop<T>(queue);
            }
            return total;
        });
    }

    // NB: provided that the time complete the tasks is much greater
    // than the time to spawn the tasks, the bootstrap time can be ignored.

    // wait for the workers thread to finish and aggregate the subtotals

    long total_pushed = 0;
    for (auto &fut: pushers) {
        total_pushed += fut.get();
    }

    long total_popped = 0;
    for (auto &fut: poppers) {
        total_popped += fut.get();
    }

    if (total_pushed != total_popped
        || total_pushed != (long) num_elems) {
        std::cerr << "Problem with queues: pushed: "
                  << total_pushed
                  << "; popped:"
                  << total_popped << "\n";
        assert(false);
    }

    return double(num_elems) / timer;
}

template<typename F>
double get_harmonic_mean(F &&callable, size_t times) {
    double s = 0;
    for (size_t i = 0; i < times; i++) {
        s += 1.0 / callable();
    }
    return double(times) / s;
}

// Do multiple runs of a single scenario; compute harmonic mean
// of the throughputs and report to cerr.
template<typename T, class Queue>
void test_scenario(
        Queue &queue,
        size_t num_pushers,
        size_t num_poppers,
        size_t num_elems) {
    auto get_throughput_once = [&queue, num_pushers, num_poppers, num_elems] {
        return get_throughput<T>(queue, num_pushers, num_poppers, num_elems);
    };

    // aggregate over a few runs
    const double throughput = get_harmonic_mean(get_throughput_once, 10);

    const auto s = std::to_string(num_pushers)
                   + "/" + std::to_string(num_poppers);

    std::cerr << std::setw(8) << std::left << s
              << std::setw(8) << std::left << std::setprecision(4) << throughput / 1e6
              << std::setw(0);

    size_t bar_size = size_t(throughput / 1e5) + 1;
    size_t bar_capacity = 100;
    for (size_t i = 0; i < std::min(bar_capacity, bar_size); i++) {
        std::cerr << "*";
    }

    std::cerr << (bar_size > bar_capacity ? "..." : "") << std::endl;
}

/////////////////////////////////////////////////////////////////////////////

template<typename T, class Queue>
void test_scenarios(Queue &queue) {
    size_t num_cores = std::thread::hardware_concurrency();
    size_t half_cores = num_cores == 1 ? 1 : num_cores / 2;
    size_t num_elems = g_num_elements;

    test_scenario<T>(queue, 1UL, 1UL, num_elems / 4); // SPSC
    test_scenario<T>(queue, 1UL, num_cores, num_elems / 4); // SPMC
    test_scenario<T>(queue, num_cores, 1UL, num_elems / 4); // MPSC
    test_scenario<T>(queue, half_cores, half_cores, num_elems);   // MPMC
    test_scenario<T>(queue, num_cores * 4, num_cores * 4, num_elems);   // MPSC(congested)
}

/////////////////////////////////////////////////////////////////////////////

// Queue has a constructor accepting capacity.
template<typename T, typename Queue>
void test(const std::string &label, Queue *) {
    std::cerr << "\n" << label << "\n";
    Queue queue{g_capacity};
    test_scenarios<T>(queue);
}

// tbb::concurrent_bounded_queue needs to have set_capacity called explicitly.
//template<typename T>
//void test(const std::string &label, tbb::concurrent_bounded_queue <T> *) {
//    std::cerr << "\n" << label << "\n";
//    tbb::concurrent_bounded_queue <T> queue{};
//    queue.set_capacity(g_capacity);
//    test_scenarios<T>(queue);
//}

// tbb::concurrent_queue does not have a contsructor accepting capacity
//template<typename T>
//void test(const std::string &label, tbb::concurrent_queue <T> *) {
//    std::cerr << "\n" << label << "\n";
//    tbb::concurrent_queue <T> queue{};
//    test_scenarios<T>(queue);
//}

/////////////////////////////////////////////////////////////////////////////

int main() {
    struct utsname uts;
    uname(&uts);

    std::cerr << "hardware concurrency: " << std::thread::hardware_concurrency()
              << "\nplatform:" << uts.sysname << " " << uts.release
              << "\nqueues capacity:" << g_capacity
              << "\nelements to pump: " << g_num_elements
              << "\ncolumns: producers/consumers | throughput(M/s) | bar"
              << std::endl;

    using T = long;

#define TEST(...) test<T>(#__VA_ARGS__, reinterpret_cast<__VA_ARGS__*>(NULL));

    TEST(mt::naive_synchronized_queue<T, std::mutex>);
    TEST(mt::naive_synchronized_queue<T, mt::atomic_lock<> >);

    TEST(mpmc_bounded_queue_t<T>);

    TEST(moodycamel::ConcurrentQueue<T>);
    TEST(moodycamel::BlockingConcurrentQueue<T>);

    TEST(rigtorp::MPMCQueue<T>);

    TEST(boost::sync_bounded_queue < T >);
    TEST(boost::lockfree::queue < T >);

    TEST(LockFreeQueue<T>);

    TEST(bbq::BlockBoundedQueue<T>);

#ifdef WITH_BOOST_FIBER
    TEST( boost::fibers::bounded_channel<T> );
#endif

    return 0;
}
