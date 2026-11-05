#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace umbriel {

  // Modifiers advertised by the primary planes' IN_FORMATS_ASYNC blob for
  // `format`, in the order the driver lists them. Empty when the driver does
  // not expose async-plane formats (or when the device is not DRM).
  std::vector<uint64_t> drmAsyncScanoutModifiers(int drmFd, uint32_t format);

  // Keep only the modifiers that appear in `preferred`, preserving the
  // preference order. Used to steer scanout allocations away from modifiers
  // the kernel rejects for async page flips (for example CCS on Intel gen9).
  std::vector<uint64_t>
  filterPreferredModifiers(const uint64_t* modifiers, std::size_t count, const std::vector<uint64_t>& preferred);

} // namespace umbriel
