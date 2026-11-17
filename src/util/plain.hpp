#pragma once
#include <type_traits>

// Plain data: copying is a memcpy, destruction is a no-op, and memory from an
// arena becomes a valid T without placement new. Rules out virtuals and owning
// types. Constructors that just set values (std::optional, default member
// initializers) are fine.
template <typename T>
concept Plain =
    std::is_trivially_copyable_v<T> && std::is_implicit_lifetime_v<T>;
