#include <infra/lock_free_queue.h>
#include "common.h"
#include <atomic>

namespace lldk 
{
namespace infra 
{

// MPSC variable-size queue inspired by Agrona ManyToOneRingBuffer.
// Bounded only. Multiple producers CAS on tail, single consumer reads head.
//
// Entry layout: [int64_t header (8 bytes)][data (ALIGN8(uSize) bytes)]
//
// Header encoding (signed interpretation):
//   0            → empty (not claimed, or freed by consumer)
//   < 0 (!= -1) → claimed, not committed; abs(header) = entry_total_size
//   > 0          → committed; header = entry_total_size
//   -1           → padding sentinel (consumer skips to buffer start)
//
// entry_total_size = kHeaderSize + ALIGN8(uSize), always >= 8, so:
//   - claimed values are <= -8 (never -1)
//   - committed values are >= 8 (never 0)
//   - padding is uniquely -1

class MPSCVariableSizeQueueImpl : public LockFreeVariableSizeQueue<QueueType::kMPSC>
{
    static constexpr uint64_t kHeaderSize = sizeof(int64_t);
    static constexpr int64_t kPaddingSentinel = -1;

public:
    MPSCVariableSizeQueueImpl(uint64_t uMemorySize)
        : m_uMemorySize(LLDK_ALIGN8(uMemorySize))
        , m_uMask(m_uMemorySize - 1)
    {
    }

    ~MPSCVariableSizeQueueImpl()
    {
        delete[] m_pBuffer;
    }

    int32_t Init()
    {
        if (m_uMemorySize == 0
            || (m_uMemorySize & m_uMask) != 0
            || m_uMemorySize == kUnlimitedMemorySize)
        {
            return -1;
        }

        m_pBuffer = LLDK_NEW uint8_t[m_uMemorySize];
        if (m_pBuffer == nullptr)
        {
            return -1;
        }

        memset(m_pBuffer, 0, m_uMemorySize);
        return 0;
    }

    // --- Producer side (multiple threads, CAS-based) ---

    void *New(uint64_t uSize)
    {
        uint64_t uAlignedSize = LLDK_ALIGN8(uSize);
        uint64_t uEntrySize = kHeaderSize + uAlignedSize;

        if (unlikely(uEntrySize > m_uMemorySize))
        {
            m_statistics.uNewFailedCount++;
            return nullptr;
        }

        uint64_t uTail = m_uTail.load(std::memory_order_relaxed);

        for (;;)
        {
            uint64_t uHead = m_uHead.load(std::memory_order_acquire);
            uint64_t uFree = m_uMemorySize - (uTail - uHead);
            uint64_t uWritePos = uTail & m_uMask;
            uint64_t uTailSpace = m_uMemorySize - uWritePos;

            if (likely(uEntrySize <= uTailSpace))
            {
                if (unlikely(uEntrySize > uFree))
                {
                    m_statistics.uNewFailedCount++;
                    return nullptr;
                }

                if (m_uTail.compare_exchange_weak(uTail, uTail + uEntrySize,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    HeaderAt(uWritePos)->store(
                        -static_cast<int64_t>(uEntrySize), std::memory_order_release);
                    m_statistics.uNewCount++;
                    return m_pBuffer + uWritePos + kHeaderSize;
                }
            }
            else
            {
                uint64_t uTotalClaim = uTailSpace + uEntrySize;
                if (unlikely(uTotalClaim > uFree))
                {
                    m_statistics.uNewFailedCount++;
                    return nullptr;
                }

                if (m_uTail.compare_exchange_weak(uTail, uTail + uTotalClaim,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    HeaderAt(0)->store(
                        -static_cast<int64_t>(uEntrySize), std::memory_order_relaxed);
                    HeaderAt(uWritePos)->store(
                        kPaddingSentinel, std::memory_order_release);
                    m_statistics.uNewCount++;
                    return m_pBuffer + kHeaderSize;
                }
            }
        }
    }

    void Post(void *pItem)
    {
        assert(pItem != nullptr);

        auto *pHeader = HeaderFromData(pItem);
        int64_t uNeg = pHeader->load(std::memory_order_relaxed);
        assert(uNeg < kPaddingSentinel);
        pHeader->store(-uNeg, std::memory_order_release);
    }

    // --- Consumer side (single thread) ---

    void *Get()
    {
        uint64_t uHead = m_uHead.load(std::memory_order_relaxed);
        uint64_t uTail = m_uTail.load(std::memory_order_acquire);

        while (uHead < uTail)
        {
            uint64_t uLocalPos = uHead & m_uMask;
            int64_t uHeader = HeaderAt(uLocalPos)->load(std::memory_order_acquire);

            if (unlikely(uHeader == kPaddingSentinel))
            {
                uint64_t uPadding = m_uMemorySize - uLocalPos;
                memset(m_pBuffer + uLocalPos, 0, uPadding);
                uHead += uPadding;
                m_uHead.store(uHead, std::memory_order_release);
                continue;
            }

            if (uHeader <= 0)
            {
                m_statistics.uGetFailedCount++;
                return nullptr;
            }

            return m_pBuffer + uLocalPos + kHeaderSize;
        }

        m_statistics.uGetFailedCount++;
        return nullptr;
    }

    void Free(void *pItem)
    {
        assert(pItem != nullptr);

        auto *pHeader = HeaderFromData(pItem);
        int64_t uEntrySize = pHeader->load(std::memory_order_relaxed);
        assert(uEntrySize > 0);

        memset(static_cast<uint8_t *>(pItem) - kHeaderSize, 0, static_cast<uint64_t>(uEntrySize));

        uint64_t uHead = m_uHead.load(std::memory_order_relaxed);
        m_uHead.store(uHead + static_cast<uint64_t>(uEntrySize), std::memory_order_release);
        m_statistics.uFreeCount++;
    }

    LLDK_INLINE uint32_t Size() const
    {
        uint64_t uTail = m_uTail.load(std::memory_order_relaxed);
        uint64_t uHead = m_uHead.load(std::memory_order_relaxed);
        return static_cast<uint32_t>(uTail - uHead);
    }

    LLDK_INLINE uint32_t Capacity() const { return static_cast<uint32_t>(m_uMemorySize); }
    LLDK_INLINE bool IsFull() const { return Size() >= m_uMemorySize; }

    LLDK_INLINE bool IsEmpty() const
    {
        return m_uTail.load(std::memory_order_relaxed) == m_uHead.load(std::memory_order_relaxed);
    }

    int32_t GetStatistics(QueueStatistics *pStatistics) const
    {
        *pStatistics = m_statistics;
        return 0;
    }

    void Clear()
    {
        m_uTail.store(0, std::memory_order_relaxed);
        m_uHead.store(0, std::memory_order_relaxed);
        memset(m_pBuffer, 0, m_uMemorySize);
        m_statistics.Reset();
    }

private:
    LLDK_INLINE std::atomic<int64_t> *HeaderAt(uint64_t uPos) const
    {
        return reinterpret_cast<std::atomic<int64_t> *>(m_pBuffer + uPos);
    }

    LLDK_INLINE std::atomic<int64_t> *HeaderFromData(void *pData) const
    {
        return reinterpret_cast<std::atomic<int64_t> *>(
            static_cast<uint8_t *>(pData) - kHeaderSize);
    }

    const uint64_t m_uMemorySize;
    const uint64_t m_uMask;

    uint8_t *m_pBuffer = nullptr;

    // --- producer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) std::atomic<uint64_t> m_uTail{0};

    // --- consumer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) std::atomic<uint64_t> m_uHead{0};

    QueueStatistics m_statistics;
};

template<>
LockFreeVariableSizeQueue<QueueType::kMPSC> *LockFreeVariableSizeQueue<QueueType::kMPSC>::Create(uint64_t uMemorySize)
{
    auto pQueue = LLDK_NEW MPSCVariableSizeQueueImpl(uMemorySize);
    if (pQueue == nullptr)
    {
        return nullptr;
    }
    if (pQueue->Init() != 0)
    {
        delete pQueue;
        return nullptr;
    }
    return static_cast<LockFreeVariableSizeQueue<QueueType::kMPSC> *>(pQueue);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kMPSC>::Destroy(LockFreeVariableSizeQueue<QueueType::kMPSC> *pQueue)
{
    delete static_cast<MPSCVariableSizeQueueImpl *>(pQueue);
}

template<>
void *LockFreeVariableSizeQueue<QueueType::kMPSC>::New(uint64_t uSize)
{
    return static_cast<MPSCVariableSizeQueueImpl *>(this)->New(uSize);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kMPSC>::Post(void *pItem)
{
    static_cast<MPSCVariableSizeQueueImpl *>(this)->Post(pItem);
}

template<>
void *LockFreeVariableSizeQueue<QueueType::kMPSC>::Get()
{
    return static_cast<MPSCVariableSizeQueueImpl *>(this)->Get();
}

template<>
void LockFreeVariableSizeQueue<QueueType::kMPSC>::Free(void *pItem)
{
    static_cast<MPSCVariableSizeQueueImpl *>(this)->Free(pItem);
}

template<>
uint32_t LockFreeVariableSizeQueue<QueueType::kMPSC>::Size() const
{
    return static_cast<const MPSCVariableSizeQueueImpl *>(this)->Size();
}

template<>
uint32_t LockFreeVariableSizeQueue<QueueType::kMPSC>::Capacity() const
{
    return static_cast<const MPSCVariableSizeQueueImpl *>(this)->Capacity();
}

template<>
bool LockFreeVariableSizeQueue<QueueType::kMPSC>::IsFull() const
{
    return static_cast<const MPSCVariableSizeQueueImpl *>(this)->IsFull();
}

template<>
bool LockFreeVariableSizeQueue<QueueType::kMPSC>::IsEmpty() const
{
    return static_cast<const MPSCVariableSizeQueueImpl *>(this)->IsEmpty();
}

template<>
int32_t LockFreeVariableSizeQueue<QueueType::kMPSC>::GetStatistics(QueueStatistics *pStatistics) const
{
    return static_cast<const MPSCVariableSizeQueueImpl *>(this)->GetStatistics(pStatistics);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kMPSC>::Clear()
{
    static_cast<MPSCVariableSizeQueueImpl *>(this)->Clear();
}

}
}
