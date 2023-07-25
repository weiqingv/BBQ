#ifndef LOCKLESS_BBQ_H
#define LOCKLESS_BBQ_H

#include <memory>
#include <thread>
#include <atomic>
#include <cmath>
#include <variant>

namespace bbq {
    template<typename T, size_t S>
    struct EntryDesc;

    struct BlockDone {
        uint64_t vsn;
    };

    struct NoEntry {
    };

    struct NotAvailable {
    };

    template<typename T, size_t S>
    struct Allocated {
        EntryDesc<T, S> e;
    };

    struct Success {
    };

    template<typename T, size_t S>
    struct Reserved {
        EntryDesc<T, S> e;
    };

    template<typename T, size_t S>
    using QueueState = std::variant<BlockDone, NoEntry, NotAvailable, Allocated<T, S>, Reserved<T, S>, Success>;

    struct Full {
    };

    struct Busy {
    };

    struct Empty {
    };

    template<typename T>
    struct OK {
        T data;
    };

    template<typename T>
    using QueueStatus = std::variant<Full, Busy, Empty, OK<T>>;

    // QueueState
    const int BLOCK_DONE = 0;
    const int NO_ENTRY = 1;
    const int NOT_AVAILABLE = 2;
    const int ALLOCATED = 3;
    const int RESERVED = 4;
    const int SUCCESS = 5;

    // QueueStatus
    const int FULL = 0;
    const int BUSY = 1;
    const int EMPTY = 2;
    const int OKAY = 3;

    using Cursor = std::atomic<uint64_t>;
    using Head = std::atomic<uint64_t>;

    enum class Policy {
        DROP_OLD,
        RETRY_NEW
    };

    constexpr unsigned floor_log2(uint64_t x) {
        return x == 1 ? 0 : 1 + floor_log2(x >> 1);
    }

    constexpr unsigned ceil_log2(uint64_t x) {
        return x == 1 ? 0 : floor_log2(x - 1) + 1;
    }

    template<typename T, size_t BS>
    class Block {
    public:
        Cursor committed, allocated;
        Cursor reserved, consumed;
        T entries[BS];

        explicit Block() : committed(0), allocated(0),
                           reserved(0), consumed(0) {}

        void init(uint64_t off) {
            committed |= off;
            allocated |= off;
            reserved |= off;
            consumed |= off;
        }
    };

    template<typename T, size_t BN = 128, size_t BS = 128>
    class BlockBoundedQueue {
    public:
        QueueStatus<T> enqueue(T &);

        QueueStatus<T> dequeue();

        explicit BlockBoundedQueue(Policy p) : phead(0), chead(0), policy(p) {
            for (size_t i = 1; i < BN; ++i) {
                auto &blk = blocks[i];
                blk.init(BS);
            }
        }

    private:
        Head phead, chead;
        Block<T, BS> blocks[BN];
        Policy policy;

        const unsigned idx_bits = ceil_log2(BN);
        const unsigned off_bits = ceil_log2(BS) + 1;
        const unsigned idx_mask = (1 << idx_bits) - 1;
        const unsigned off_mask = (1 << off_bits) - 1;

        QueueState<T, BS> allocate_entry(Block<T, BS> &);

        void commit_entry(EntryDesc<T, BS> &, T &);

        QueueState<T, BS> advance_phead(uint64_t);

        QueueState<T, BS> reserve_entry(Block<T, BS> &);

        T *consume_entry(EntryDesc<T, BS> &);

        bool advance_chead(uint64_t, uint64_t);

        inline uint64_t idx(uint64_t x) { return x & idx_mask; }

        inline uint64_t head_vsn(uint64_t x) { return x >> idx_bits; }

        inline uint64_t off(uint64_t x) { return x & off_mask; }

        inline uint64_t cur_vsn(uint64_t x) { return x >> off_bits; }

        inline uint64_t set_cur_vsn(uint64_t ver) { return ver << off_bits; }
    };

    template<typename T, size_t S>
    struct EntryDesc {
        Block<T, S> *block;
        uint64_t off;
        uint64_t vsn;
    };

    inline uint64_t fetch_max(std::atomic<uint64_t> &atom, uint64_t upd) {
        auto now = atom.load(std::memory_order_acquire);
        while (!atom.compare_exchange_weak(now, std::max(now, upd),
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire))
            now = atom.load(std::memory_order_acquire);

        return now;
    }

    template<typename T, size_t BN, size_t BS>
    QueueState<T, BS> BlockBoundedQueue<T, BN, BS>::allocate_entry(Block<T, BS> &block) {
        if (off(block.allocated.load(std::memory_order_acquire)) >= BS)
            return BlockDone{};

        auto old = block.allocated.fetch_add(1, std::memory_order_acq_rel);
        if (off(old) >= BS)
            return BlockDone{};

        return Allocated<T, BS>{.e={.block=&block, .off=off(old), .vsn=cur_vsn(old)}};
    }

    template<typename T, size_t BN, size_t BS>
    void BlockBoundedQueue<T, BN, BS>::commit_entry(EntryDesc<T, BS> &e, T &data) {
        e.block->entries[e.off] = std::move(data);
        e.block->committed.fetch_add(1, std::memory_order_acq_rel);
    }

    template<typename T, size_t BN, size_t BS>
    QueueState<T, BS> BlockBoundedQueue<T, BN, BS>::advance_phead(uint64_t ph) {
        Block<T, BS> &n_blk = blocks[(idx(ph) + 1) % BN];

        uint64_t cur, reserved;
        switch (policy) {
            case Policy::RETRY_NEW:
                // consumed
                cur = n_blk.consumed.load(std::memory_order_acquire);
                if (cur_vsn(cur) < head_vsn(ph) || (cur_vsn(cur) == head_vsn(ph) && off(cur) != BS)) {
                    reserved = n_blk.reserved.load(std::memory_order_acquire);
                    if (off(reserved) == off(cur)) return NoEntry{};
                    else return NotAvailable{};
                }
                break;
            case Policy::DROP_OLD:
                // committed
                cur = n_blk.committed.load(std::memory_order_acquire);
                if (cur_vsn(cur) == head_vsn(ph) && off(cur) != BS)
                    return NotAvailable{};
                break;
            default:
                break;
        }

        fetch_max(n_blk.committed, set_cur_vsn(head_vsn(ph) + 1));
        fetch_max(n_blk.allocated, set_cur_vsn(head_vsn(ph) + 1));
        fetch_max(phead, ph + 1);

        return Success{};
    }

    template<typename T, size_t BN, size_t BS>
    QueueState<T, BS> BlockBoundedQueue<T, BN, BS>::reserve_entry(Block<T, BS> &block) {
        while (true) {
            auto reserved = block.reserved.load(std::memory_order_acquire);
            if (off(reserved) < BS) {
                auto committed = block.committed.load(std::memory_order_acquire);
                if (off(committed) == off(reserved))
                    return NoEntry{};
                if (off(committed) != BS) {
                    auto allocated = block.allocated.load(std::memory_order_acquire);
                    if (off(allocated) != off(committed))
                        return NotAvailable{};
                }

                if (fetch_max(block.reserved, reserved + 1) == reserved)
                    return Reserved<T, BS>{.e={.block=&block, .off=off(reserved), .vsn=cur_vsn(reserved)}};
                else
                    continue;
            }
            return BlockDone{.vsn=cur_vsn(reserved)};
        }
    }

    template<typename T, size_t BN, size_t BS>
    T *BlockBoundedQueue<T, BN, BS>::consume_entry(EntryDesc<T, BS> &e) {
        T *addr = &e.block->entries[e.off];

        uint64_t allocated;
        switch (policy) {
            case Policy::RETRY_NEW:
                e.block->consumed.fetch_add(1, std::memory_order_acq_rel);
                break;
            case Policy::DROP_OLD:
                allocated = e.block->allocated.load(std::memory_order_acquire);
                if (cur_vsn(allocated) != e.vsn)
                    return nullptr;
                break;
            default:
                break;
        }

        return addr;
    }

    template<typename T, size_t BN, size_t BS>
    bool BlockBoundedQueue<T, BN, BS>::advance_chead(uint64_t ch, uint64_t ver) {
        Block<T, BS> &n_blk = blocks[(idx(ch) + 1) % BN];
        auto committed = n_blk.committed.load(std::memory_order_acquire);

        switch (policy) {
            case Policy::RETRY_NEW:
                if (cur_vsn(committed) != head_vsn(ch) + 1)
                    return false;
                fetch_max(n_blk.consumed, set_cur_vsn(head_vsn(ch) + 1));
                fetch_max(n_blk.reserved, set_cur_vsn(head_vsn(ch) + 1));
                break;
            case Policy::DROP_OLD:
                if (cur_vsn(committed) < ver + (idx(ch) == 0))
                    return false;
                fetch_max(n_blk.reserved, set_cur_vsn(cur_vsn(committed)));
                break;
            default:
                break;
        }

        fetch_max(chead, ch + 1);
        return true;
    }

    template<typename T, size_t BN, size_t BS>
    QueueStatus<T> BlockBoundedQueue<T, BN, BS>::enqueue(T &data) {
        while (true) {
            auto ph = phead.load(std::memory_order_acquire);
            auto &blk = blocks[idx(ph)];

            QueueState<T, BS> ps;
            auto state = allocate_entry(blk);
            switch (state.index()) {
                case ALLOCATED:
                    commit_entry(std::get_if<Allocated<T, BS>>(&state)->e, data);
                    return OK<T>{};
                case BLOCK_DONE:
                    ps = advance_phead(ph);
                    switch (ps.index()) {
                        case NO_ENTRY:
                            return Full{};
                        case NOT_AVAILABLE:
                            return Busy{};
                        case SUCCESS:
                            continue;
                    }
                    break;
                default:
                    throw std::range_error("Invalid QueueState in enqueue: " + std::to_string(state.index()));
            }
        }
    }

    template<typename T, size_t BN, size_t BS>
    QueueStatus<T> BlockBoundedQueue<T, BN, BS>::dequeue() {
        while (true) {
            auto ch = chead.load(std::memory_order_acquire);
            auto &blk = blocks[idx(ch)];

            T *data;
            auto state = reserve_entry(blk);
            switch (state.index()) {
                case RESERVED:
                    data = consume_entry(std::get_if<Reserved<T, BS>>(&state)->e);
                    if (data) return OK<T>{.data=*data};
                    else continue;
                case NO_ENTRY:
                    return Empty{};
                case NOT_AVAILABLE:
                    return Busy{};
                case BLOCK_DONE:
                    if (advance_chead(ch, std::get_if<BlockDone>(&state)->vsn)) continue;
                    else return Empty{};
                default:
                    throw std::range_error("Invalid QueueState in dequeue: " + std::to_string(state.index()));
            }
        }
    }
}

#endif //LOCKLESS_BBQ_H
