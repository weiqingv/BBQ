#include <iostream>
#include <thread>
#include <atomic>
#include <variant>
#include <chrono>
#include "lockfree_queue.h"
#include "bbq.h"

using namespace bbq;

void test_lock_free_queue() {
    const int M = 65536;
    lockfree_queue<uint16_t> q;
    std::atomic<uint16_t> seq{0};
    std::atomic<int> finished_producer{0};

    static const int PRODUCE_N = 4;     // num of producers
    static const int CONSUME_N = 4;     // num of consumers
    static const int MULTIPLIER = 1000;

    auto producer = [&q, &seq, &finished_producer]() {
        for (int i = 0; i < M * MULTIPLIER; ++i) {
            uint16_t s = seq++;
            while (!q.enqueue(s));
        }
        finished_producer++;
    };

    std::atomic<uint32_t> counter[M];
    for (auto &i: counter) i = 0;

    auto consumer = [&q, &counter, &finished_producer]() {
        uint16_t s = 0;
        while (finished_producer < PRODUCE_N) {
            if (q.dequeue(s))
                counter[s]++;
        }

        while (q.dequeue(s)) {
            counter[s]++;
        }
    };

    auto start = std::chrono::system_clock::now();

    // create producer and consumer threads
    std::unique_ptr<std::thread> produce_threads[PRODUCE_N];
    std::unique_ptr<std::thread> consumer_threads[CONSUME_N];
    for (auto &consumer_thread: consumer_threads) {
        consumer_thread = std::make_unique<std::thread>(consumer);
    }
    for (auto &produce_thread: produce_threads) {
        produce_thread = std::make_unique<std::thread>(producer);
    }

    // join threads
    for (auto &produce_thread: produce_threads)
        produce_thread->join();
    for (auto &consumer_thread: consumer_threads)
        consumer_thread->join();

    auto end = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Elapsed time: " << duration << "ms" << std::endl;

    bool has_race = false;
    for (int i = 0; i < M; ++i) {
        if (counter[i] != MULTIPLIER * PRODUCE_N) {
            std::cout << "Found race condition " << i << " " << counter[i] << std::endl;
            has_race = true;
            break;
        }
    }

    if (!has_race)
        std::cout << "No race condition" << std::endl;
}

void test_bbq() {
    const int M = 65536;
    BlockBoundedQueue<uint16_t, 8, 65536> q(Policy::RETRY_NEW);
    std::atomic<uint16_t> seq{0};
    std::atomic<int> finished_producer{0};

    static const int PRODUCE_N = 4;     // num of producers
    static const int CONSUME_N = 4;     // num of consumers
    static const int MULTIPLIER = 1000;

    auto producer = [&q, &seq, &finished_producer]() {
        QueueStatus<uint16_t> stat;
        for (int i = 0; i < M * MULTIPLIER; ++i) {
            uint16_t s = seq++;
            while (q.enqueue(s).index() != OKAY);
        }
        finished_producer++;
    };

    std::atomic<uint32_t> counter[M];
    for (auto &i: counter) i = 0;

    auto consumer = [&q, &counter, &finished_producer]() {
        QueueStatus<uint16_t> stat;
        while (finished_producer < PRODUCE_N) {
            stat = q.dequeue();
            if (stat.index() == OKAY) {
                auto s = std::get_if<OK<uint16_t>>(&stat)->data;
                counter[s]++;
            }
        }

        QueueStatus<uint16_t> res = q.dequeue();
        while (res.index() == OKAY) {
            auto s = std::get_if<OK<uint16_t>>(&res)->data;
            counter[s]++;
            res = q.dequeue();
        }
    };

    auto start = std::chrono::system_clock::now();

    // create producer and consumer threads
    std::unique_ptr<std::thread> produce_threads[PRODUCE_N];
    std::unique_ptr<std::thread> consumer_threads[CONSUME_N];
    for (auto &consumer_thread: consumer_threads) {
        consumer_thread = std::make_unique<std::thread>(consumer);
    }
    for (auto &produce_thread: produce_threads) {
        produce_thread = std::make_unique<std::thread>(producer);
    }

    // join threads
    for (auto &produce_thread: produce_threads)
        produce_thread->join();
    for (auto &consumer_thread: consumer_threads)
        consumer_thread->join();

    auto end = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Elapsed time: " << duration << "ms" << std::endl;

    bool has_race = false;
    int cnt = 0;
    for (int i = 0; i < M; ++i) {
        if (counter[i] != MULTIPLIER * PRODUCE_N) {
            std::cout << "Found race condition: " << i << " " << counter[i] << std::endl;
            has_race = true;
            cnt++;
        }
    }

    if (!has_race)
        std::cout << "No race condition" << std::endl;
    else
        std::cout << "Race condition num: " << cnt << std::endl;
}

void enqueue(BlockBoundedQueue<int, 5, 5> &queue, int num) {
    for (int i = 0; i < num; i++) {
        int data = 500 + i;
        queue.enqueue(data);
    }
}

void dequeue(BlockBoundedQueue<int, 5, 5> &queue, int num) {
    for (int i = 0; i < num; i++) {
        auto res = queue.dequeue();
        switch (res.index()) {
            case EMPTY:
                std::cout << "Empty" << std::endl;
                break;
            case OKAY:
                std::cout << std::get_if<OK<int>>(&res)->data << std::endl;
                break;
        }
    }
}

void test_single() {
    BlockBoundedQueue<int, 5, 5> queue(Policy::RETRY_NEW);

    enqueue(queue, 25);
    dequeue(queue, 25);
    std::cout << "============================================================" << std::endl;
    enqueue(queue, 25);
    dequeue(queue, 25);
    std::cout << "============================================================" << std::endl;
    enqueue(queue, 25);
    dequeue(queue, 25);
    std::cout << "============================================================" << std::endl;
}

int main() {
//    test_single();
    test_lock_free_queue();
    test_bbq();

    return 0;
}