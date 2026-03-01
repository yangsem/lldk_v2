#include <infra/lock_free_queue.h>
#include "common.h"

namespace lldk 
{
namespace infra 
{

class SPSCVariableSizeQueueImpl : public LockFreeVariableSizeQueue<QueueType::kSPSC>
{
    static constexpr uint64_t kDefaultBlockMemorySize = 1024 * 1024;

    struct Block
    {
        Block *pNext = nullptr;
        uint8_t data[0];
    };

public:
    SPSCVariableSizeQueueImpl(uint64_t uMemorySize)
        : m_uBlockMemorySize(uMemorySize == kUnlimitedMemorySize ? kDefaultBlockMemorySize : LLDK_ALIGN8(uMemorySize))
        , m_uBlockMask(m_uBlockMemorySize - 1)
        , m_bUnlimited(uMemorySize == kUnlimitedMemorySize)
    {
    }

    ~SPSCVariableSizeQueueImpl()
    {
        Block *pBlock = m_pHeadBlock;
        while (pBlock != nullptr)
        {
            Block *pNext = pBlock->pNext;
            delete[] (uint8_t *)pBlock;
            pBlock = pNext;
        }
    }

    int32_t Init()
    {
        if (m_uBlockMemorySize == 0
            || (m_uBlockMemorySize & m_uBlockMask) != 0)
        {
            return -1;
        }

        Block *pBlock = AllocateBlock();
        if (pBlock == nullptr)
        {
            return -1;
        }

        m_pHeadBlock = pBlock;
        m_pWriteBlock = pBlock;
        m_pReadBlock = pBlock;
        m_uWriteBlockEnd = m_uBlockMemorySize;
        m_uReadBlockEnd = m_uBlockMemorySize;

        return 0;
    }

    void *New(uint64_t uSize)
    {
        uint64_t uAlignedSize = LLDK_ALIGN8(uSize);
        uint64_t uEntrySize = sizeof(uint64_t) + uAlignedSize;

        if (!m_bUnlimited)
        {
            // === Limited mode: single block ring buffer ===
            uint64_t uFree = m_uBlockMemorySize - (m_uTail - m_uHead);
            if (unlikely(uEntrySize > uFree))
            {
                m_statistics.uNewFailedCount++;
                return nullptr;
            }

            uint64_t uWritePos = m_uTail & m_uBlockMask;
            uint64_t uReadPos = m_uHead & m_uBlockMask;

            if (uWritePos >= uReadPos)
            {
                // [....head===data===tail........end]
                // free: [tail, end) + [0, head)
                uint64_t uTailSpace = m_uBlockMemorySize - uWritePos;

                if (likely(uEntrySize <= uTailSpace))
                {
                    *reinterpret_cast<uint64_t *>(m_pWriteBlock->data + uWritePos) = uAlignedSize;
                    m_statistics.uNewCount++;
                    return m_pWriteBlock->data + uWritePos + sizeof(uint64_t);
                }

                if (unlikely(uEntrySize > uReadPos))
                {
                    m_statistics.uNewFailedCount++;
                    return nullptr;
                }

                *reinterpret_cast<uint64_t *>(m_pWriteBlock->data + uWritePos) = UINT64_MAX;
                m_uTail += uTailSpace;

                *reinterpret_cast<uint64_t *>(m_pWriteBlock->data) = uAlignedSize;
                m_statistics.uNewCount++;
                return m_pWriteBlock->data + sizeof(uint64_t);
            }

            // [===tail..........head===data===end]
            // free: [tail, head), contiguous, no wrap needed
            *reinterpret_cast<uint64_t *>(m_pWriteBlock->data + uWritePos) = uAlignedSize;
            m_statistics.uNewCount++;
            return m_pWriteBlock->data + uWritePos + sizeof(uint64_t);
        }

        // === Unlimited mode: multi-block, linear within each block ===
        if (unlikely(uEntrySize > m_uBlockMemorySize))
        {
            m_statistics.uNewFailedCount++;
            return nullptr;
        }

        if (unlikely(m_uTail >= m_uWriteBlockEnd))
        {
            if (unlikely(!AdvanceWriteBlock()))
            {
                m_statistics.uNewFailedCount++;
                return nullptr;
            }
        }

        uint64_t uLocalPos = m_uTail & m_uBlockMask;
        uint64_t uRemaining = m_uBlockMemorySize - uLocalPos;

        if (likely(uEntrySize <= uRemaining))
        {
            *reinterpret_cast<uint64_t *>(m_pWriteBlock->data + uLocalPos) = uAlignedSize;
            m_statistics.uNewCount++;
            return m_pWriteBlock->data + uLocalPos + sizeof(uint64_t);
        }

        // Entry doesn't fit at tail of current block, pad and advance
        *reinterpret_cast<uint64_t *>(m_pWriteBlock->data + uLocalPos) = UINT64_MAX;
        m_uTail += uRemaining;

        if (unlikely(!AdvanceWriteBlock()))
        {
            m_statistics.uNewFailedCount++;
            return nullptr;
        }

        *reinterpret_cast<uint64_t *>(m_pWriteBlock->data) = uAlignedSize;
        m_statistics.uNewCount++;
        return m_pWriteBlock->data + sizeof(uint64_t);
    }

    void Post(void *pItem)
    {
        assert(pItem != nullptr);
        assert((uintptr_t)pItem >= (uintptr_t)m_pWriteBlock->data
            && (uintptr_t)pItem < (uintptr_t)m_pWriteBlock->data + m_uBlockMemorySize);

        uint64_t uAlignedSize = *reinterpret_cast<uint64_t *>(
            static_cast<uint8_t *>(pItem) - sizeof(uint64_t));
        m_statistics.uPostCount++;
        m_uTail += sizeof(uint64_t) + uAlignedSize;
    }

    void *Get()
    {
        if (likely(m_uHead != m_uTail))
        {
            if (unlikely(m_bUnlimited && m_uHead >= m_uReadBlockEnd))
            {
                m_pReadBlock = m_pReadBlock->pNext;
                m_uReadBlockEnd += m_uBlockMemorySize;
            }

            uint64_t uLocalPos = m_uHead & m_uBlockMask;
            uint64_t uHeader = *reinterpret_cast<uint64_t *>(m_pReadBlock->data + uLocalPos);

            if (unlikely(uHeader == UINT64_MAX))
            {
                m_uHead += m_uBlockMemorySize - uLocalPos;

                if (m_bUnlimited && m_uHead >= m_uReadBlockEnd)
                {
                    m_pReadBlock = m_pReadBlock->pNext;
                    m_uReadBlockEnd += m_uBlockMemorySize;
                }

                if (unlikely(m_uHead == m_uTail))
                {
                    m_statistics.uGetFailedCount++;
                    return nullptr;
                }

                return m_pReadBlock->data + sizeof(uint64_t);
            }

            return m_pReadBlock->data + uLocalPos + sizeof(uint64_t);
        }

        m_statistics.uGetFailedCount++;
        return nullptr;
    }

    void Free(void *pItem)
    {
        assert(pItem != nullptr);
        assert((uintptr_t)pItem >= (uintptr_t)m_pReadBlock->data
            && (uintptr_t)pItem < (uintptr_t)m_pReadBlock->data + m_uBlockMemorySize);

        uint64_t uAlignedSize = *reinterpret_cast<uint64_t *>(
            static_cast<uint8_t *>(pItem) - sizeof(uint64_t));
        m_statistics.uFreeCount++;
        m_uHead += sizeof(uint64_t) + uAlignedSize;
    }

    LLDK_INLINE uint32_t Size() const { return static_cast<uint32_t>(m_uTail - m_uHead); }
    LLDK_INLINE uint32_t Capacity() const { return m_bUnlimited ? kUnlimitedCapacity : static_cast<uint32_t>(m_uBlockMemorySize); }
    LLDK_INLINE bool IsFull() const { return !m_bUnlimited && m_uTail - m_uHead >= m_uBlockMemorySize; }
    LLDK_INLINE bool IsEmpty() const { return m_uHead == m_uTail; }
    int32_t GetStatistics(QueueStatistics *pStatistics) const { *pStatistics = m_statistics; return 0; }

    void Clear()
    {
        m_uHead = 0;
        m_uTail = 0;
        m_pWriteBlock = m_pHeadBlock;
        m_pReadBlock = m_pHeadBlock;
        m_uWriteBlockEnd = m_uBlockMemorySize;
        m_uReadBlockEnd = m_uBlockMemorySize;
        m_statistics.Reset();
    }

private:
    Block *AllocateBlock()
    {
        auto ptr = LLDK_NEW uint8_t[m_uBlockMemorySize + sizeof(Block)];
        if (ptr == nullptr)
        {
            return nullptr;
        }

        auto *pBlock = reinterpret_cast<Block *>(ptr);
        pBlock->pNext = nullptr;
        return pBlock;
    }

    bool AdvanceWriteBlock()
    {
        // 1. Reuse pre-linked block (e.g. blocks surviving a Clear())
        if (m_pWriteBlock->pNext != nullptr)
        {
            m_pWriteBlock = m_pWriteBlock->pNext;
            m_uWriteBlockEnd += m_uBlockMemorySize;
            return true;
        }

        // 2. Recycle a fully-consumed block from the head of the chain
        if (m_pHeadBlock != m_pReadBlock)
        {
            Block *pRecycled = m_pHeadBlock;
            m_pHeadBlock = m_pHeadBlock->pNext;
            pRecycled->pNext = nullptr;
            m_pWriteBlock->pNext = pRecycled;
            m_pWriteBlock = pRecycled;
            m_uWriteBlockEnd += m_uBlockMemorySize;
            return true;
        }

        // 3. Allocate a fresh block
        Block *pNew = AllocateBlock();
        if (pNew == nullptr) return false;
        m_pWriteBlock->pNext = pNew;
        m_pWriteBlock = pNew;
        m_uWriteBlockEnd += m_uBlockMemorySize;
        return true;
    }

    const uint64_t m_uBlockMemorySize;
    const uint64_t m_uBlockMask;
    const bool m_bUnlimited;

    Block *m_pHeadBlock = nullptr;

    // --- producer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) uint64_t m_uTail = 0;
    Block *m_pWriteBlock = nullptr;
    uint64_t m_uWriteBlockEnd = 0;

    // --- consumer-hot state, isolated on its own cache line ---
    alignas(LLDK_CACHELINE_SIZE) uint64_t m_uHead = 0;
    Block *m_pReadBlock = nullptr;
    uint64_t m_uReadBlockEnd = 0;

    QueueStatistics m_statistics;
};

template<>
LockFreeVariableSizeQueue<QueueType::kSPSC> *LockFreeVariableSizeQueue<QueueType::kSPSC>::Create(uint64_t uMemorySize)
{
    auto pQueue = LLDK_NEW SPSCVariableSizeQueueImpl(uMemorySize);
    if (pQueue == nullptr)
    {
        return nullptr;
    }
    if (pQueue->Init() != 0)
    {
        delete pQueue;
        return nullptr;
    }
    return static_cast<LockFreeVariableSizeQueue<QueueType::kSPSC> *>(pQueue);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kSPSC>::Destroy(LockFreeVariableSizeQueue<QueueType::kSPSC> *pQueue)
{
    delete static_cast<SPSCVariableSizeQueueImpl *>(pQueue);
}

template<>
void *LockFreeVariableSizeQueue<QueueType::kSPSC>::New(uint64_t uSize)
{
    return static_cast<SPSCVariableSizeQueueImpl *>(this)->New(uSize);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kSPSC>::Post(void *pItem)
{
    static_cast<SPSCVariableSizeQueueImpl *>(this)->Post(pItem);
}

template<>
void *LockFreeVariableSizeQueue<QueueType::kSPSC>::Get()
{
    return static_cast<SPSCVariableSizeQueueImpl *>(this)->Get();
}

template<>
void LockFreeVariableSizeQueue<QueueType::kSPSC>::Free(void *pItem)
{
    static_cast<SPSCVariableSizeQueueImpl *>(this)->Free(pItem);
}

template<>
uint32_t LockFreeVariableSizeQueue<QueueType::kSPSC>::Size() const
{
    return static_cast<const SPSCVariableSizeQueueImpl *>(this)->Size();
}

template<>
uint32_t LockFreeVariableSizeQueue<QueueType::kSPSC>::Capacity() const
{
    return static_cast<const SPSCVariableSizeQueueImpl *>(this)->Capacity();
}

template<>
bool LockFreeVariableSizeQueue<QueueType::kSPSC>::IsFull() const
{
    return static_cast<const SPSCVariableSizeQueueImpl *>(this)->IsFull();
}

template<>
bool LockFreeVariableSizeQueue<QueueType::kSPSC>::IsEmpty() const
{
    return static_cast<const SPSCVariableSizeQueueImpl *>(this)->IsEmpty();
}

template<>
int32_t LockFreeVariableSizeQueue<QueueType::kSPSC>::GetStatistics(QueueStatistics *pStatistics) const
{
    return static_cast<const SPSCVariableSizeQueueImpl *>(this)->GetStatistics(pStatistics);
}

template<>
void LockFreeVariableSizeQueue<QueueType::kSPSC>::Clear()
{
    static_cast<SPSCVariableSizeQueueImpl *>(this)->Clear();
}

}
}
