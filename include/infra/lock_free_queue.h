#ifndef __LLDK_INFRA_LOCK_FREE_QUEUE_H__
#define __LLDK_INFRA_LOCK_FREE_QUEUE_H__

#include <cstdint>
#include <cstring>

namespace lldk
{
namespace infra
{

enum class QueueType : uint8_t
{
    kSPSC = 0,
    kSPMC,
    kMPSC,
    kMPMC,
    kMax
};

struct QueueStatistics
{
    uint64_t uNewCount = 0;
    uint64_t uNewFailedCount = 0;
    uint64_t uPostCount = 0;
    uint64_t uPostFailedCount = 0;
    uint64_t uGetCount = 0;
    uint64_t uGetFailedCount = 0;
    uint64_t uFreeCount = 0;
    uint64_t uFreeFailedCount = 0;

    void Reset()
    {
        memset(this, 0, sizeof(*this));
    }
};

constexpr uint32_t kUnlimitedCapacity = UINT32_MAX;

template <QueueType kType>
class LockFreeFixedSizeQueueBase
{
public:
    LockFreeFixedSizeQueueBase() = default;
    ~LockFreeFixedSizeQueueBase() = default;

    LockFreeFixedSizeQueueBase(const LockFreeFixedSizeQueueBase&) = delete;
    LockFreeFixedSizeQueueBase(LockFreeFixedSizeQueueBase&&) = delete;
    LockFreeFixedSizeQueueBase& operator=(const LockFreeFixedSizeQueueBase&) = delete;
    LockFreeFixedSizeQueueBase& operator=(LockFreeFixedSizeQueueBase&&) = delete;

    static LockFreeFixedSizeQueueBase* Create(uint32_t uSizeOfT, uint32_t uCapacity);
    static void Destroy(LockFreeFixedSizeQueueBase *pQueue);

    void *New();
    void Post(void *pItem);

    void *Get();
    void Free(void *pItem);

    uint32_t Size() const;
    uint32_t Capacity() const;
    bool IsFull() const;
    bool IsEmpty() const;

    int32_t GetStatistics(QueueStatistics *pStatistics) const;

    void Clear();
};

template<typename T, QueueType kType>
class LockFreeFixedSizeQueue : public LockFreeFixedSizeQueueBase<kType>
{
public:
    static LockFreeFixedSizeQueue* Create(uint32_t uCapacity)
    {
        return static_cast<LockFreeFixedSizeQueue*>(LockFreeFixedSizeQueueBase<kType>::Create(sizeof(T), uCapacity));
    }

    static void Destroy(LockFreeFixedSizeQueue *pQueue)
    {
        LockFreeFixedSizeQueueBase<kType>::Destroy(pQueue);
    }

    int32_t Push(const T &item)
    {
        auto pItem = LockFreeFixedSizeQueueBase<kType>::New();
        if (pItem == nullptr)
        {
            return -1;
        }
        new (pItem) T(item);
        LockFreeFixedSizeQueueBase<kType>::Post(pItem);
        return 0;
    }

    int32_t Pop(T &item)
    {
        auto pItem = LockFreeFixedSizeQueueBase<kType>::Get();
        if (pItem == nullptr)
        {
            return -1;
        }
        item = *static_cast<T*>(pItem);
        LockFreeFixedSizeQueueBase<kType>::Free(pItem);
        return 0;
    }
};

constexpr uint64_t kUnlimitedMemorySize = UINT64_MAX;

template <QueueType kType>
class LockFreeVariableSizeQueue
{
public:
LockFreeVariableSizeQueue() = default;
    ~LockFreeVariableSizeQueue() = default;

    LockFreeVariableSizeQueue(const LockFreeVariableSizeQueue&) = delete;
    LockFreeVariableSizeQueue(LockFreeVariableSizeQueue&&) = delete;
    LockFreeVariableSizeQueue& operator=(const LockFreeVariableSizeQueue&) = delete;
    LockFreeVariableSizeQueue& operator=(LockFreeVariableSizeQueue&&) = delete;

    static LockFreeVariableSizeQueue* Create(uint64_t uMemorySize);
    static void Destroy(LockFreeVariableSizeQueue *pQueue);

    void *New(uint64_t uSize);
    void Post(void *pItem);

    void *Get();
    void Free(void *pItem);

    uint32_t Size() const;
    uint32_t Capacity() const;
    bool IsFull() const;
    bool IsEmpty() const;

    void Clear();

    int32_t GetStatistics(QueueStatistics *pStatistics) const;
};

}
}

#endif // __LLDK_INFRA_LOCK_FREE_QUEUE_H__
