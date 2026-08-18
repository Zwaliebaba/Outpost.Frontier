#pragma once

#include <type_traits>

namespace Neuron
{
#define ENUM_HELPER(T, S, E)                                                                        \
  T inline operator++(T& _value) noexcept                                                           \
  {                                                                                                 \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) + 1);             \
  }                                                                                                 \
  T inline operator++(T& _value, int) noexcept                                                      \
  {                                                                                                 \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) + 1);             \
  }                                                                                                 \
  T inline operator--(T& _value) noexcept                                                           \
  {                                                                                                 \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) - 1);             \
  }                                                                                                 \
  T inline operator--(T& _value, int) noexcept                                                      \
  {                                                                                                 \
    return _value = static_cast<T>(static_cast<std::underlying_type_t<T>>(_value) - 1);             \
  }                                                                                                 \
  inline T operator*(T _type) noexcept                                                              \
  {                                                                                                 \
    return _type;                                                                                   \
  }                                                                                                 \
  constexpr size_t SizeOf##T() noexcept                                                             \
  {                                                                                                 \
    return static_cast<size_t>(T::E) + 1;                                                           \
  }                                                                                                 \
  class It##T                                                                                       \
  {                                                                                                 \
    int value;                                                                                      \
                                                                                                    \
  public:                                                                                           \
    explicit It##T(int v)                                                                           \
      : value(v)                                                                                    \
    {                                                                                               \
    }                                                                                               \
    ##T operator*() const                                                                           \
    {                                                                                               \
      return static_cast<##T>(value);                                                               \
    }                                                                                               \
    bool operator!=(const It##T& other) const                                                       \
    {                                                                                               \
      return value != other.value;                                                                  \
    }                                                                                               \
    It##T& operator++()                                                                             \
    {                                                                                               \
      ++value;                                                                                      \
      return *this;                                                                                 \
    }                                                                                               \
  };                                                                                                \
  class Range##T                                                                                    \
  {                                                                                                 \
  public:                                                                                           \
    It##T begin() const                                                                             \
    {                                                                                               \
      return It##T(static_cast<std::underlying_type_t<T>>(T::S));                                   \
    }                                                                                               \
    It##T end() const                                                                               \
    {                                                                                               \
      return It##T(static_cast<std::underlying_type_t<T>>(T::E) + 1);                               \
    }                                                                                               \
  };                                                                                                \
  constexpr T operator|(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) | ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator|=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) |= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr T operator&(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) & ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator&=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) &= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr T operator~(T a) noexcept                                                               \
  {                                                                                                 \
    return T(~((_ENUM_FLAG_SIZED_INTEGER<T>::type)a));                                              \
  }                                                                                                 \
  constexpr T operator^(T a, T b) noexcept                                                          \
  {                                                                                                 \
    return T(((_ENUM_FLAG_SIZED_INTEGER<T>::type)a) ^ ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b));      \
  }                                                                                                 \
  inline T& operator^=(T& a, T b) noexcept                                                          \
  {                                                                                                 \
    return (T&)(((_ENUM_FLAG_SIZED_INTEGER<T>::type&)a) ^= ((_ENUM_FLAG_SIZED_INTEGER<T>::type)b)); \
  }                                                                                                 \
  constexpr bool operator!(T a) noexcept                                                            \
  {                                                                                                 \
    return !((_ENUM_FLAG_SIZED_INTEGER<T>::type)a);                                                 \
  }
//template <> struct std::formatter<T> : std::formatter<int> {                                                                                     \
  //  auto format(const T& id, std::format_context& ctx) const {return std::formatter<int>::format(static_cast<int>(id), ctx); }                     \
  //}

template <typename T> constexpr bool IsValidEnum(T _value) noexcept
{
  return (_value >= begin(_value) && _value < end(_value));
}

template <typename T> constexpr size_t I(T _value) noexcept
{
  return static_cast<size_t>(_value);
}

class BaseException : public std::exception
{
public:
  BaseException(std::string s) noexcept
    : m_s(std::move(s))
  {
  }
  ~BaseException() noexcept override = default;

  [[nodiscard]] const char* what() const noexcept override
  {
    return m_s.c_str();
  }

protected:
  std::string m_s;
};

struct NonCopyable
{
  NonCopyable() = default;
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
};

struct handle_closer
{
  void operator()(HANDLE h) const noexcept
  {
    if (h)
      CloseHandle(h);
  }
};

using ScopedHandle = std::unique_ptr<void, handle_closer>;
inline HANDLE safe_handle(HANDLE h) noexcept
{
  return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
}
} // namespace Neuron
