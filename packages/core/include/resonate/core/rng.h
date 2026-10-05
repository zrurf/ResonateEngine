#ifndef RESONATE_CORE_RNG_H
#define RESONATE_CORE_RNG_H

#include <cstddef>
#include <cstdint>

namespace resonate
{

/* Deterministic pseudo-random values, splitmix64. The engine's replayability
   rests on this being a pure function of its state: the same seed and the same
   consumption order give the same values, on every platform and build.

   A stream is one state: a system owns its stream and consumes it alone, so
   parallel work branches per slice (branch()) instead of sharing one. */
class Rng
{
  public:
    /* Any 64 bits are a legal state, including zero. */
    explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t nextU64() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::uint32_t nextU32() noexcept
    {
        return static_cast<std::uint32_t>(nextU64() >> 32);
    }

    /* Uniform [0, 1), in steps of 2^-24. */
    float nextFloat01() noexcept
    {
        return static_cast<float>(nextU32() >> 8) * (1.0F / 16777216.0F);
    }

    /* Uniform integer in [0, bound); zero bound reads as zero. Lemire's
       rejection: no modulo skew, and the expected rejection count is below
       one. */
    std::uint64_t nextBelow(std::uint64_t bound) noexcept
    {
        if (bound < 2U)
        {
            return 0U;
        }
        const std::uint64_t threshold = (0ULL - bound) % bound;
        for (;;)
        {
            const std::uint64_t value = nextU64();
            if (value >= threshold)
            {
                return value % bound;
            }
        }
    }

    std::uint32_t nextBelow(std::uint32_t bound) noexcept
    {
        return static_cast<std::uint32_t>(nextBelow(static_cast<std::uint64_t>(bound)));
    }

    /* A sub-stream that runs beside this one: no shared state, so parallel
       slices consume their own. Branching is deterministic in the salt. */
    [[nodiscard]] Rng branch(std::uint64_t salt) const noexcept
    {
        Rng branched(state_ ^ (salt * 0xD1B54A32D192ED03ULL));
        branched.nextU64();
        branched.nextU64();
        return branched;
    }

    [[nodiscard]] std::uint64_t state() const noexcept
    {
        return state_;
    }

  private:
    std::uint64_t state_;
};

} // namespace resonate

#endif /* RESONATE_CORE_RNG_H */
