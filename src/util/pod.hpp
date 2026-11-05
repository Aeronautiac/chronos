#pragma once

// plain old data

#include <type_traits>
template <typename T>
concept Pod = std::is_trivially_default_constructible_v<T> &&
              std::is_trivially_copyable_v<T>;
