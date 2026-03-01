#include <infra/lock_free_queue.h>
#include "common.h"
#include <cassert>

namespace lldk 
{
namespace infra 
{

class SPSCFixedSizeQueueImpl : public LockFreeFixedSizeQueueBase<QueueType::kSPSC>
{
    static constexpr uint32_t kDefaultBlockCapacity = 1024 * 1024;

    struct Block
    {
        Block *pNext = nullptr;
        uint8_t data[0];
    };

public:
    SPSCFixedSizeQueueImpl(uint32_t uSizeOfT, uint32_t uCapacity)
        : m_uSizeOfT(LLDK_ALIGN8(uSizeOfT))
        , m_uBlockCapacity(uCapacity == kUnlimitedCapacity ? kDefaultBlockCapacity : uCapacity)
        , m_uBlockMask(m_uBlockCapacity - 1)
        , m_bUnlimited(uCapacity == kUnlimitedCapacity)
    {
    }

    ~SPSCFixedSizeQueueImpl()
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
        if (m_uSizeOfT == 0 || m_uBlockCapacity == 0
            || (m_uBlockCapacity & m_uBlockMask) != 0)
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
        m_uWriteBlockEnd = m_uBlockCapacity;
        m_uReadBlockEnd = m_uBlockCapacity;

        return 0;
    }

    LLDK_INLINE void *New()
    {
        if (!m_bUnlimited)
        {
            if (likely(m_uTail - m_uHead < m_uBlockCapacity))
            {
                m_statistics.uNewCount++;
                uint32_t uSlot = static_cast<uint32_t>(m_uTail) & m_uBlockMask;
                return m_pWriteBlock->data + uSlot * m_uSizeOfT;
            }
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

        m_statistics.uNewCount++;
        uint32_t uSlot = static_cast<uint32_t>(m_uTail) & m_uBlockMask;
        return m_pWriteBlock->data + uSlot * m_uSizeOfT;
    }

    LLDK_INLINE void Post(void *pItem)
    {
        assert(pItem != nullptr);
        assert((uintptr_t)pItem >= (uintptr_t)m_pWriteBlock->data
            && (uintptr_t)pItem < (uintptr_t)m_pWriteBlock->data + m_uBlockCapacity * m_uSizeOfT);

        m_statistics.uPostCount++;
        m_uTail++;
    }

    LLDK_INLINE void *Get()
    {
        if (likely(m_uHead != m_uTail))
        {
            if (unlikely(m_bUnlimited && m_uHead >= m_uReadBlockEnd))
            {
                m_pReadBlock = m_pReadBlock->pNext;
                m_uReadBlockEnd += m_uBlockCapacity;
            }

            uint32_t uSlot = static_cast<uint32_t>(m_uHead) & m_uBlockMask;
            return m_pReadBlock->data + uSlot * m_uSizeOfT;
        }

        m_statistics.uGetFailedCount++;
        return nullptr;
    }

    LLDK_INLINE void Free(void *pItem)
    {
        assert(pItem != nullptr);
        assert((uintptr_t)pItem >= (uintptr_t)m_pReadBlock->data
            && (uintptr_t)pItem < (uintptr_t)m_pReadBlock->data + m_uBlockCapacity * m_uSizeOfT);

        m_statistics.uFreeCount++;
        m_uHead++;
    }

    LLDK_INLINE uint32_t Size() const { return static_cast<uint32_t>(m_uTail - m_uHead); }
    LLDK_INLINE uint32_t Capacity() const { return m_bUnlimited ? kUnlimitedCapacity : m_uBlockCapacity; }
    LLDK_INLINE bool IsFull() const { return !m_bUnlimited && m_uTail - m_uHead >= m_uBlockCapacity; }
    LLDK_INLINE bool IsEmpty() const { return m_uHead == m_uTail; }
    int32_t GetStatistics(QueueStatistics *pStatistics) const { *pStatistics = m_statistics; return 0; }

    void Clear()
    {
        m_uHead = 0;
        m_uTail = 0;
        m_pWriteBlock = m_pHeadBlock;
        m_pReadBlock = m_pHeadBlock;
        m_uWriteBlockEnd = m_uBlockCapacity;
        m_uReadBlockEnd = m_uBlockCapacity;
        m_statistics.Reset();
    }

private:
    Block *AllocateBlock()
    {
        auto ptr = LLDK_NEW uint8_t[m_uBlockCapacity * m_uSizeOfT + sizeof(Block)];
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
            m_uWriteBlockEnd += m_uBlockCapacity;
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
            m_uWriteBlockEnd += m_uBlockCapacity;
            return true;
        }

        // 3. Allocate a fresh block
        Block *pNew = AllocateBlock();
        if (pNew == nullptr) return false;
        m_pWriteBlock->pNext = pNew;
        m_pWriteBlock = pNew;
        m_uWriteBlockEnd += m_uBlockCapacity;
        return true;
    }

    const uint32_t m_uSizeOfT;
    const uint32_t m_uBlockCapacity;
    const uint32_t m_uBlockMask;
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
LockFreeFixedSizeQueueBase<QueueType::kSPSC> *LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Create(uint32_t uSizeOfT, uint32_t uCapacity)
{
    auto pQueue = LLDK_NEW SPSCFixedSizeQueueImpl(uSizeOfT, uCapacity);
    if (pQueue == nullptr)
    {
        return nullptr;
    }
    if (pQueue->Init() != 0)
    {
        delete pQueue;
        return nullptr;
    }
    return static_cast<LockFreeFixedSizeQueueBase<QueueType::kSPSC> *>(pQueue);
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Destroy(LockFreeFixedSizeQueueBase<QueueType::kSPSC> *pQueue)
{
    delete static_cast<SPSCFixedSizeQueueImpl *>(pQueue);
}

template<>
void *LockFreeFixedSizeQueueBase<QueueType::kSPSC>::New()
{
    return static_cast<SPSCFixedSizeQueueImpl *>(this)->New();
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Post(void *pItem)
{
    static_cast<SPSCFixedSizeQueueImpl *>(this)->Post(pItem);
}

template<>
void *LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Get()
{
    return static_cast<SPSCFixedSizeQueueImpl *>(this)->Get();
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Free(void *pItem)
{
    static_cast<SPSCFixedSizeQueueImpl *>(this)->Free(pItem);
}

template<>
uint32_t LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Size() const
{
    return static_cast<const SPSCFixedSizeQueueImpl *>(this)->Size();
}

template<>
uint32_t LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Capacity() const
{
    return static_cast<const SPSCFixedSizeQueueImpl *>(this)->Capacity();
}

template<>
bool LockFreeFixedSizeQueueBase<QueueType::kSPSC>::IsFull() const
{
    return static_cast<const SPSCFixedSizeQueueImpl *>(this)->IsFull();
}

template<>
bool LockFreeFixedSizeQueueBase<QueueType::kSPSC>::IsEmpty() const
{
    return static_cast<const SPSCFixedSizeQueueImpl *>(this)->IsEmpty();
}

template<>
int32_t LockFreeFixedSizeQueueBase<QueueType::kSPSC>::GetStatistics(QueueStatistics *pStatistics) const
{
    return static_cast<const SPSCFixedSizeQueueImpl *>(this)->GetStatistics(pStatistics);
}

template<>
void LockFreeFixedSizeQueueBase<QueueType::kSPSC>::Clear()
{
    static_cast<SPSCFixedSizeQueueImpl *>(this)->Clear();
}

}
}
