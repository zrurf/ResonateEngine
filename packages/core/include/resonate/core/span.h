#ifndef RESONATE_CORE_SPAN_H
#define RESONATE_CORE_SPAN_H

#include <cstddef>

namespace resonate
{

/* Non-owning view over a contiguous range. */
template <typename T> class Span
{
  public:
    using value_type = T;
    using iterator = T*;

    constexpr Span() = default;
    constexpr Span(T* data, std::size_t size) noexcept : data_(data), size_(size)
    {
    }

    template <std::size_t N> constexpr Span(T (&array)[N]) noexcept : data_(array), size_(N)
    {
    }

    [[nodiscard]] constexpr T* data() const noexcept
    {
        return data_;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept
    {
        return size_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return size_ == 0;
    }

    constexpr T* begin() const noexcept
    {
        return data_;
    }
    constexpr T* end() const noexcept
    {
        return data_ + size_;
    }

    constexpr T& operator[](std::size_t index) const noexcept
    {
        return data_[index];
    }

    /* Clamped: an offset at or past the end yields an empty span, and count is
       limited to what remains. */
    [[nodiscard]] constexpr Span subspan(std::size_t offset, std::size_t count) const noexcept
    {
        if (offset >= size_)
        {
            return Span();
        }
        const std::size_t remaining = size_ - offset;
        return Span(data_ + offset, count < remaining ? count : remaining);
    }

  private:
    T* data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace resonate

#endif /* RESONATE_CORE_SPAN_H */
