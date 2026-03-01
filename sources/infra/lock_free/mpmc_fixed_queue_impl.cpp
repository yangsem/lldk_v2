#include <infra/lock_free_queue.h>
#include "common.h"
#include <atomic>

namespace lldk 
{
namespace infra 
{

// Vyukov bounded MPMC queue with interleaved single-array layout.
// Both producers and consumers use CAS on their respective cursors.
// Reference: rigtorp/MPMCQueue, Dmitry Vyukov's bounded MPMC queue.
class MPMCFixedSizeQueueImpl : public LockFreeFixedSizeQueueBase<QueueType::kMPMC>
{
    static constexpr uint32_t kSequenceSize = sizeof(std::atomic<uint64_t>);

public:
    MPMCFixedSizeQueueImpl(uint32_t uSizeOfT, uint32_t uCapacity)
        : m_uSizeOfT(LLDK_ALIGN8(uSizeOfT))
        , m_uSlotStride(kSequenceSize + LLDK_ALIGN8(uSizeOfT))
        , m_uCapacity(uCapacity)
        , m_uMask(uCapacity - 1)
    {
    }

    ~MPMCFixedSizeQueueImpl()
    {
        if (m_pSlots != nullptr)
        {
            for (uint32_t i = 0; i < m_uCapacity; i++)
            {
                SequenceAt(i)->~atomic();
            }
            delete[] m_pSlots;
        }
    }

    int32_t Init()
    {
        if (m_uSizeOfT == 0 || m_uCapacity == 0
            || (m_uCapacity & m_uMask) != 0
            || m_uCapacity == kUnlimitedCapacity)
        {
            return -1;
        }

        m_pSlots = LLDK_NEW uint8_t[m_uCapacity * m_uSlotStride];
        if (m_pSlots == nullptr)
        {
            return -1;
        }

        for (uint32_t i = 0; i < m_uCapacity; i++)
        {
            new (SlotBase(i)) std::atomic<uint64_t>(static_cast<uint64_t>(i));
        }

        return 0;
    }

    // --- Producer side (multiple threads, CAS-based) ---

    LLDK_INLINE void *New()
    {
        uint64_t uPos = m_uTail.load(std::memory_order_relaxed);

        for (;;)
        {
            uint32_t uSlot = static_cast<uint32_t>(uPos) & m_uMask;
            uint64_t uSeq = SequenceAt(uSlot)->load(std::memory_order_acquire);
            int64_t uDiff = static_cast<int64_t>(uSeq) - static_cast<int64_t>(uPos);

            if (uDiff == 0)
            {
                if (m_uTail.compare_exchange_weak(uPos, uPos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    m_statistics.uNewCount++;
                    return DataAt(uSlot);
                }
            }
            else if (uDiff < 0)
            {
                m_statistics.uNewFailedCount++;
                return nullptr;
            }
            else
            {
                uPos = m_uTail.load(std::memory_order_relaxed);
            }
        }
    }

    LLDK_INLINE void Post(void *pItem)
    {
        assert(pItem != nullptr);

        uint32_t uSlot = static_cast<uint32_t>(
            static_cast<uint8_t *>(pItem) - m_pSlots - kSequenceSize) / m_uSlotStride;
        uint64_t uSeq = SequenceAt(uSlot)->load(std::memory_order_relaxed);
        SequenceAt(uSlot)->store(uSeq + 1, std::memory_order_release);
    }

    // --- Consumer side (multiple threads, CAS-based) ---

    LLDK_INLINE void *Get()
    {
        uint64_t uPos = m_uHead.load(std::memory_order_relaxed);

        for (;;)
        {
            uint32_t uSlot = static_cast<uint32_t>(uPos) & m_uMask;
            uint64_t uSeq = SequenceAt(uSlot)->load(std::memory_order_acquire);
            int64_t uDiff = static_cast<int64_t>(uSeq) - static_cast<int64_t>(uPos + 1);

            if (uDiff == 0)
            {
                if (m_uHead.compare_exchange_weak(uPos, uPos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    return DataAt(uSlot);
                }
            }
            else if (uDiff < 0)
            {
                m_statistics.uGetFailedCount++;
                return nullptr;
            }
            else
            {
                uPos = m_uHead.load(std::memory_order_relaxed);
            }
        }
    }

    LLDK_INLINE void Free(void *pItem)
    {
        assert(pItem != nullptr);

        uint32_t uSlot = static_cast<uint32_t>(
            static_cast<uint8_t *>(pItem) - m_pSlots - kSequenceSize) / m_uSlotStride;
        uint64_t uSeq = SequenceAt(uSlot)->load(std::memory_order_relaxed);
        SequenceAt(uSlot)->store(uSeq + m_uMask, std::memory_order_release);
        m_statistics.uFreeCount++;
    }

    LLDK_INLINE uint32_t Size() const
    {
        uint64_t uTail = m_uTail.load(std::memory_order_relaxed);
        uint64_t uHead = m_uHead.load(std::memory_order_relaxed);
        return static_cast<uint32_t>(uTail - uHead);
    }
    LLDK_INLINE uint32_t Capacity() const { return m_uCapacity; }
    LLDK_INLINE bool IsFull() const { return Size() >= m_uCapacity; }
    LLDK_INLINE bool IsEmpty() const
    {
        return m_uTail.load(std::memory_order_relaxed) == m_uHead.load(std::memory_order_relaxed);
    }
    int32_t GetStatistics(QueueStatistics *pStatistics) const { *pStatistics = m_statistics; return 0; }

    void Clear()
    {
        m_uTail.store(0, std::memory_order_relaxed);
        m_uHead.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < m_uCapacity; i++)
        {
            SequenceAt(i)->store(static_cast<uint64_t>(i), std::memory_order_relaxed);
        }
        m_statistics.Reset();
    }

private:
    LLDK_INLINE uint8_t *SlotBase(uint32_t uSlot) const
    {
        return m_pSlots + uSlot * m_uSlotStride;
    }

    LLDK_INLINE std::atomic<uint64_t> *SequenceAt(uint32_t uSlot) const
    {
        return reinterpret_cast<std::atomic<uint64_t> *>(SlotBase(uSlot));
    }

    LLDK_INLINE uint8_t *DataAt(uint32_t uSlot) const
    {
        return SlotBase(uSlot) + kSequenceSize;
    }

    const uint32_t m_uSizeOfT;
    const uint32_t m_uSlotStride;
    const uint32_t m_uCapacity;
    const uint32_t m_uMask;

    uint8_t *m_pSlots = nullptr;

    // --- producer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) std::atomic<uint64_t> m_uTail{0};

    // --- consumer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) std::atomic<uint64_t> m_uHead{0};

    QueueStatistics m_statistics;
};

template<>
LockFreeFixedSizeQueueBase<QueueType::kMPMC> *LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Create(uint32_t uSizeOfT, uint32_t uCapacity)
{
    auto pQueue = LLDK_NEW MPMCFixedSizeQueueImpl(uSizeOfT, uCapacity);
    if (pQueue == nullptr)
    {
        return nullptr;
    }
    if (pQueue->Init() != 0)
    {
        delete pQueue;
        return nullptr;
    }
    return static_cast<LockFreeFixedSizeQueueBase<QueueType::kMPMC> *>(pQueue);
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Destroy(LockFreeFixedSizeQueueBase<QueueType::kMPMC> *pQueue)
{
    delete static_cast<MPMCFixedSizeQueueImpl *>(pQueue);
}

template<>
void *LockFreeFixedSizeQueueBase<QueueType::kMPMC>::New()
{
    return static_cast<MPMCFixedSizeQueueImpl *>(this)->New();
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Post(void *pItem)
{
    static_cast<MPMCFixedSizeQueueImpl *>(this)->Post(pItem);
}

template<>
void *LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Get()
{
    return static_cast<MPMCFixedSizeQueueImpl *>(this)->Get();
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Free(void *pItem)
{
    static_cast<MPMCFixedSizeQueueImpl *>(this)->Free(pItem);
}

template<>
uint32_t LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Size() const
{
    return static_cast<const MPMCFixedSizeQueueImpl *>(this)->Size();
}

template<>
uint32_t LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Capacity() const
{
    return static_cast<const MPMCFixedSizeQueueImpl *>(this)->Capacity();
}

template<>
bool LockFreeFixedSizeQueueBase<QueueType::kMPMC>::IsFull() const
{
    return static_cast<const MPMCFixedSizeQueueImpl *>(this)->IsFull();
}

template<>
bool LockFreeFixedSizeQueueBase<QueueType::kMPMC>::IsEmpty() const
{
    return static_cast<const MPMCFixedSizeQueueImpl *>(this)->IsEmpty();
}

template<>
int32_t LockFreeFixedSizeQueueBase<QueueType::kMPMC>::GetStatistics(QueueStatistics *pStatistics) const
{
    return static_cast<const MPMCFixedSizeQueueImpl *>(this)->GetStatistics(pStatistics);
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kMPMC>::Clear()
{
    static_cast<MPMCFixedSizeQueueImpl *>(this)->Clear();
}

}
}
