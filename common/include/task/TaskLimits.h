#pragma once
#include <cstddef>

namespace GCRP {
// Raw UTF-8 bytes; worst-case JSON escaping still fits the 1 MiB wire limit.
inline constexpr std::size_t kMaxTaskMetadataBytes = 128 * 1024;
inline constexpr std::size_t kMaxWorkerIdBytes = 256;
inline constexpr std::size_t kMaxWorkerHostnameBytes = 1024;
} // namespace GCRP
