#ifndef ADDRESS_H
#define ADDRESS_H

#include <algorithm>
#include <cassert>
#include <ios>
#include <iomanip>
#include <iostream>

#include "util/bits.h"

namespace champsim {
template <std::size_t UPPER, std::size_t LOWER=UPPER>
class address_slice;
inline constexpr std::size_t dynamic_extent = std::numeric_limits<std::size_t>::max();

template <std::size_t UP, std::size_t LOW>
[[nodiscard]] constexpr auto offset(address_slice<UP, LOW> base, address_slice<UP, LOW> other) -> typename address_slice<UP, LOW>::difference_type;

template <std::size_t UP, std::size_t LOW>
[[nodiscard]] constexpr auto splice(address_slice<UP, LOW> upper, address_slice<UP, LOW> lower, std::size_t bits) -> address_slice<UP, LOW>;

template <std::size_t UP, std::size_t LOW>
[[nodiscard]] constexpr auto splice(address_slice<UP, LOW> upper, address_slice<UP, LOW> lower, std::size_t bits_up, std::size_t bits_low) -> address_slice<UP, LOW>;

template <std::size_t UP_A, std::size_t LOW_A, std::size_t UP_B, std::size_t LOW_B>
[[nodiscard]] constexpr auto splice(address_slice<UP_A, LOW_A> lhs, address_slice<UP_B, LOW_B> rhs);

namespace detail {
template <typename self_type>
class address_slice_impl
{
  public:
    using underlying_type = uint64_t;
    using difference_type = int64_t;

    underlying_type value{};

    constexpr address_slice_impl() = default; // TODO remove this
    constexpr explicit address_slice_impl(underlying_type val) : value(val) {}

    constexpr static int bits = std::numeric_limits<underlying_type>::digits;

    friend std::ostream& operator<<(std::ostream& stream, const self_type &addr)
    {
      auto addr_flags = std::ios_base::hex | std::ios_base::showbase | std::ios_base::internal;
      auto addr_mask  = std::ios_base::basefield | std::ios_base::showbase | std::ios_base::adjustfield;

      auto oldflags = stream.setf(addr_flags, addr_mask);
      auto oldfill = stream.fill('0');

      stream << addr.template to<underlying_type>();

      stream.setf(oldflags, addr_mask);
      stream.fill(oldfill);

      return stream;
    }

    template <typename T>
    [[nodiscard]] constexpr T to() const
    {
      static_assert(std::is_integral_v<T>);
      if (value > std::numeric_limits<T>::max())
        throw std::domain_error{"Contained value overflows the target type"};
      if (static_cast<T>(value) < std::numeric_limits<T>::min())
        throw std::domain_error{"Contained value underflows the target type"};
      return static_cast<T>(value);
    }

    [[nodiscard]] constexpr bool operator==(self_type other) const
    {
      const self_type& derived = static_cast<const self_type&>(*this);
      if (derived.upper != other.upper)
        throw std::invalid_argument{"Upper bounds do not match"};
      if (derived.lower != other.lower)
        throw std::invalid_argument{"Lower bounds do not match"};
      return value == other.value;
    }

    [[nodiscard]] constexpr bool operator< (self_type other) const
    {
      const self_type& derived = static_cast<const self_type&>(*this);
      if (derived.upper != other.upper)
        throw std::invalid_argument{"Upper bounds do not match"};
      if (derived.lower != other.lower)
        throw std::invalid_argument{"Lower bounds do not match"};
      return value < other.value;
    }

    [[nodiscard]] constexpr bool operator!=(self_type other) const { return !(*this == other); }
    [[nodiscard]] constexpr bool operator<=(self_type other) const { return *this < other || *this == other; }
    [[nodiscard]] constexpr bool operator> (self_type other) const { return !(value <= other.value); }
    [[nodiscard]] constexpr bool operator>=(self_type other) const { return *this > other || *this == other; }

    constexpr self_type& operator+=(difference_type delta)
    {
      self_type& derived = static_cast<self_type&>(*this);
      value = static_cast<underlying_type>(value + delta);
      value &= bitmask(derived.upper - derived.lower);
      return derived;
    }

    constexpr self_type& operator-=(difference_type delta) { return operator+=(-delta); }

    [[nodiscard]] constexpr self_type operator+(difference_type delta) const
    {
      self_type retval = static_cast<const self_type&>(*this);
      retval += delta;
      return retval;
    }

    [[nodiscard]] constexpr self_type operator-(difference_type delta) const { return operator+(-delta); }

    template <std::size_t slice_upper, std::size_t slice_lower, typename D = self_type, std::enable_if_t<D::is_static, bool> = true>
    [[nodiscard]] constexpr auto slice() const -> address_slice<D::lower + slice_upper, D::lower + slice_lower>
    {
      const self_type& derived = static_cast<const self_type&>(*this);
      static_assert(slice_lower <= (D::upper - D::lower));
      static_assert(slice_upper <= (D::upper - D::lower));
      return address_slice<D::lower + slice_upper, D::lower + slice_lower>{derived};
    }

    template <std::size_t new_lower, typename D = self_type, std::enable_if_t<D::is_static, bool> = true>
    [[nodiscard]] constexpr auto slice_upper() const -> address_slice<D::upper - D::lower, new_lower>
    {
      return slice<D::upper - D::lower, new_lower, D>();
    }

    template <std::size_t new_upper, typename D = self_type, std::enable_if_t<D::is_static, bool> = true>
    [[nodiscard]] constexpr auto slice_lower() const -> address_slice<new_upper, 0>
    {
      return slice<new_upper, 0, D>();
    }

    [[nodiscard]] constexpr address_slice<dynamic_extent, dynamic_extent> slice(std::size_t slice_upper, std::size_t slice_lower) const;
    [[nodiscard]] constexpr address_slice<dynamic_extent, dynamic_extent> slice_lower(std::size_t new_upper) const;
    [[nodiscard]] constexpr address_slice<dynamic_extent, dynamic_extent> slice_upper(std::size_t new_lower) const;
};
}

template <>
class address_slice<dynamic_extent, dynamic_extent> : public detail::address_slice_impl<address_slice<dynamic_extent, dynamic_extent>>
{
  using self_type = address_slice<dynamic_extent, dynamic_extent>;
  using impl_type = detail::address_slice_impl<self_type>;

  constexpr static bool is_static = false;

  std::size_t upper{impl_type::bits};
  std::size_t lower{0};

  template <std::size_t U, std::size_t L> friend class address_slice;
  friend impl_type;

  template <std::size_t U, std::size_t L>
    friend constexpr auto splice(address_slice<U, L> upper, address_slice<U, L> lower, std::size_t bits) -> address_slice<U, L>;

  template <std::size_t UP_A, std::size_t LOW_A, std::size_t UP_B, std::size_t LOW_B>
    friend constexpr auto splice(address_slice<UP_A, LOW_A> lhs, address_slice<UP_B, LOW_B> rhs);

  public:
  using typename impl_type::underlying_type;
  using typename impl_type::difference_type;

  constexpr address_slice() = default;

  constexpr explicit address_slice(underlying_type val) : address_slice(impl_type::bits, 0, val) {}

  template <std::size_t other_up, std::size_t other_low,
           typename test_up = std::enable_if_t<other_up != dynamic_extent, void>,
           typename test_low = std::enable_if_t<other_low != dynamic_extent, void>>
  constexpr explicit address_slice(address_slice<other_up, other_low> val) : address_slice(other_up, other_low, val) {}

  template <std::size_t other_up, std::size_t other_low>
  constexpr address_slice(std::size_t up, std::size_t low, address_slice<other_up, other_low> val) : impl_type(((val.value << val.lower) & bitmask(up, low)) >> low), upper(up), lower(low)
  {
    assert(up >= low);
    assert(up <= impl_type::bits);
    assert(low <= impl_type::bits);
  }

  constexpr address_slice(std::size_t up, std::size_t low, underlying_type val) : impl_type(val & bitmask(up-low)), upper(up), lower(low)
  {
    assert(up >= low);
    assert(up <= impl_type::bits);
    assert(low <= impl_type::bits);
  }

  [[nodiscard]] constexpr std::size_t upper_extent() const { return upper; }
  [[nodiscard]] constexpr std::size_t lower_extent() const { return lower; }
};

template <std::size_t UP, std::size_t LOW>
class address_slice : public detail::address_slice_impl<address_slice<UP, LOW>>
{
  using self_type = address_slice<UP, LOW>;
  using impl_type = detail::address_slice_impl<self_type>;

  constexpr static bool is_static = true;
  constexpr static std::size_t upper{UP};
  constexpr static std::size_t lower{LOW};

  template <std::size_t U, std::size_t L> friend class address_slice;
  friend impl_type;

  static_assert(UP != LOW, "An address slice of zero width is probably a bug");
  static_assert(LOW <= UP);
  static_assert(UP <= impl_type::bits);
  static_assert(LOW <= impl_type::bits);

  template <std::size_t U, std::size_t L>
    friend constexpr auto splice(address_slice<U, L> upper, address_slice<U, L> lower, std::size_t bits) -> address_slice<U, L>;

  template <std::size_t UP_A, std::size_t LOW_A, std::size_t UP_B, std::size_t LOW_B>
    friend constexpr auto splice(address_slice<UP_A, LOW_A> lhs, address_slice<UP_B, LOW_B> rhs);

  public:
  using typename impl_type::underlying_type;
  using typename impl_type::difference_type;

  constexpr address_slice() = default;

  constexpr explicit address_slice(underlying_type val) : impl_type(val & bitmask(upper-lower)) {}

  template <std::size_t other_up, std::size_t other_low>
  constexpr explicit address_slice(address_slice<other_up, other_low> val) : address_slice(((val.value << val.lower) & bitmask(upper, lower)) >> lower) {}

  [[nodiscard]] constexpr static std::size_t upper_extent() { return upper; }
  [[nodiscard]] constexpr static std::size_t lower_extent() { return lower; }
};

template <std::size_t UP, std::size_t LOW>
address_slice(std::size_t, std::size_t, address_slice<UP, LOW>) -> address_slice<dynamic_extent, dynamic_extent>;

address_slice(std::size_t, std::size_t, detail::address_slice_impl<address_slice<dynamic_extent, dynamic_extent>>::underlying_type) -> address_slice<dynamic_extent, dynamic_extent>;

template <typename self_type>
constexpr auto detail::address_slice_impl<self_type>::slice(std::size_t slice_upper, std::size_t slice_lower) const -> address_slice<dynamic_extent, dynamic_extent>
{
  const self_type& derived = static_cast<const self_type&>(*this);
  assert(slice_lower <= (derived.upper - derived.lower));
  assert(slice_upper <= (derived.upper - derived.lower));
  return address_slice<dynamic_extent, dynamic_extent>{slice_upper + derived.lower, slice_lower + derived.lower, derived};
}

template <typename self_type>
constexpr auto detail::address_slice_impl<self_type>::slice_lower(std::size_t new_upper) const -> address_slice<dynamic_extent, dynamic_extent>
{
  return slice(new_upper, 0);
}

template <typename self_type>
constexpr auto detail::address_slice_impl<self_type>::slice_upper(std::size_t new_lower) const -> address_slice<dynamic_extent, dynamic_extent>
{
  const self_type& derived = static_cast<const self_type&>(*this);
  return slice(derived.upper-derived.lower, new_lower);
}

template <std::size_t UP, std::size_t LOW>
constexpr auto offset(address_slice<UP, LOW> base, address_slice<UP, LOW> other) -> typename address_slice<UP, LOW>::difference_type
{
  using underlying_type = typename address_slice<UP, LOW>::underlying_type;
  using difference_type = typename address_slice<UP, LOW>::difference_type;

  underlying_type abs_diff = (base.value > other.value) ? (base.value - other.value) : (other.value - base.value);
  assert(abs_diff <= std::numeric_limits<difference_type>::max());
  return (base.value > other.value) ? -static_cast<difference_type>(abs_diff) : static_cast<difference_type>(abs_diff);
}

template <std::size_t UP, std::size_t LOW>
constexpr auto splice(address_slice<UP, LOW> upper, address_slice<UP, LOW> lower, std::size_t bits) -> address_slice<UP, LOW>
{
  return splice(upper, lower, bits, 0);
}

template <std::size_t UP, std::size_t LOW>
constexpr auto splice(address_slice<UP, LOW> upper, address_slice<UP, LOW> lower, std::size_t bits_up, std::size_t bits_low) -> address_slice<UP, LOW>
{
  return address_slice<UP,LOW>{splice(upper, lower.slice(bits_up, bits_low))};
}

template <std::size_t UP_A, std::size_t LOW_A, std::size_t UP_B, std::size_t LOW_B>
constexpr auto splice(address_slice<UP_A, LOW_A> lhs, address_slice<UP_B, LOW_B> rhs)
{
  if constexpr (decltype(lhs)::is_static && decltype(rhs)::is_static) {
    constexpr auto upper_extent{std::max<std::size_t>(lhs.upper, rhs.upper)};
    constexpr auto lower_extent{std::min<std::size_t>(lhs.lower, rhs.lower)};
    using rettype = address_slice<upper_extent, lower_extent>;
    return rettype{splice_bits(rettype{lhs}.value, rettype{rhs}.value, rhs.upper - lower_extent, rhs.lower - lower_extent)};
  } else {
    const auto upper_extent{std::max<std::size_t>(lhs.upper, rhs.upper)};
    const auto lower_extent{std::min<std::size_t>(lhs.lower, rhs.lower)};
    using rettype = address_slice<dynamic_extent, dynamic_extent>;
    return rettype{upper_extent, lower_extent, splice_bits(rettype{upper_extent, lower_extent, lhs}.value, rettype{upper_extent, lower_extent, rhs}.value, rhs.upper - lower_extent, rhs.lower - lower_extent)};
  }
}
}

#endif
