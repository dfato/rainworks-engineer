#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace ve
{
    // Single-writer / single-reader lock-free "latest value" channel.
    // The writer (audio thread) never blocks and never allocates; the reader
    // (UI) always gets the most recent complete value.
    template <typename T>
    class TripleBuffer
    {
        static_assert (std::is_trivially_copyable_v<T>, "TripleBuffer needs a trivially copyable type");

    public:
        TripleBuffer() = default;

        void write (const T& value) noexcept
        {
            slots[back] = value;
            // Swap back buffer with the middle one and flag it as fresh.
            const auto prev = middle.exchange (static_cast<std::uint8_t> (back | freshBit), std::memory_order_acq_rel);
            back = prev & indexMask;
        }

        // Returns true if a newer value was available.
        bool read (T& out) noexcept
        {
            if ((middle.load (std::memory_order_relaxed) & freshBit) != 0)
            {
                const auto prev = middle.exchange (front, std::memory_order_acq_rel);
                front = prev & indexMask;
                out = slots[front];
                return true;
            }
            out = slots[front];
            return false;
        }

    private:
        static constexpr std::uint8_t freshBit  = 0x4;
        static constexpr std::uint8_t indexMask = 0x3;

        std::array<T, 3> slots {};
        std::uint8_t back = 0;                 // writer-owned
        std::uint8_t front = 1;                // reader-owned
        std::atomic<std::uint8_t> middle { 2 };
    };
}
