#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#pragma pack(push, 1)
struct H264FrameHeader
{
    std::uint32_t magic;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t dataSize;
};
#pragma pack(pop)

struct H264EncodedPacket
{
    H264FrameHeader header{};
    std::vector<unsigned char> payload;
    bool isKeyFrame = false;
};

// Single-producer/single-consumer circular buffer. Producer insertion is
// nonblocking: if its short critical section is busy, it drops the new packet.
// When full, the oldest waiting packet is overwritten by the newest one.
class H264FrameRingBuffer
{
public:
    static constexpr std::size_t Capacity = 2;

    H264FrameRingBuffer() = default;
    H264FrameRingBuffer(const H264FrameRingBuffer&) = delete;
    H264FrameRingBuffer& operator=(const H264FrameRingBuffer&) = delete;

    bool TryPush(H264EncodedPacket&& packet, bool& droppedOldest)
    {
        droppedOldest = false;
        H264EncodedPacket discarded{};
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            return false;
        }

        if (size == Capacity)
        {
            discarded = std::move(slots[head]);
            slots[head] = std::move(packet);
            head = (head + 1) % Capacity;
            droppedOldest = true;
        }
        else
        {
            const std::size_t tail = (head + size) % Capacity;
            slots[tail] = std::move(packet);
            ++size;
        }

        sizeSnapshot.store(size, std::memory_order_release);
        lock.unlock();
        notEmpty.notify_one();
        return true;
    }

    bool WaitPop(H264EncodedPacket& packet, const std::atomic<bool>& stopping)
    {
        std::unique_lock<std::mutex> lock(mutex);
        notEmpty.wait(lock, [&]
        {
            return stopping.load(std::memory_order_acquire) || size != 0;
        });

        if (stopping.load(std::memory_order_acquire) || size == 0)
        {
            return false;
        }

        packet = std::move(slots[head]);
        slots[head] = H264EncodedPacket{};
        head = (head + 1) % Capacity;
        --size;
        sizeSnapshot.store(size, std::memory_order_release);
        return true;
    }

    std::size_t ApproximateSize() const noexcept
    {
        return sizeSnapshot.load(std::memory_order_acquire);
    }

    void Clear()
    {
        std::array<H264EncodedPacket, Capacity> discarded{};
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t index = 0; index < size; ++index)
            {
                const std::size_t slot = (head + index) % Capacity;
                discarded[index] = std::move(slots[slot]);
            }
            head = 0;
            size = 0;
            sizeSnapshot.store(0, std::memory_order_release);
        }
    }

    void NotifyAll() noexcept
    {
        notEmpty.notify_all();
    }

private:
    mutable std::mutex mutex;
    std::condition_variable notEmpty;
    std::array<H264EncodedPacket, Capacity> slots{};
    std::size_t head = 0;
    std::size_t size = 0;
    std::atomic<std::size_t> sizeSnapshot{ 0 };
};
