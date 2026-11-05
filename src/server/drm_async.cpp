#include "server/drm_async.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
}

namespace umbriel {
  namespace {

    // Look up one property value by name on an already-fetched object.
    // Returns false when the property is absent or has no value.
    bool findProperty(
        int drmFd, const drmModeObjectProperties* properties, const char* name, uint32_t* propertyId, uint64_t* value
    ) {
      if (properties == nullptr) {
        return false;
      }
      for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyRes* property = drmModeGetProperty(drmFd, properties->props[i]);
        if (property == nullptr) {
          continue;
        }
        const bool match = std::string_view(property->name) == name;
        const uint64_t current = properties->prop_values[i];
        drmModeFreeProperty(property);
        if (!match) {
          continue;
        }
        if (propertyId != nullptr) {
          *propertyId = properties->props[i];
        }
        if (value != nullptr) {
          *value = current;
        }
        return true;
      }
      return false;
    }

    void appendUnique(std::vector<uint64_t>& values, uint64_t value) {
      for (const uint64_t existing : values) {
        if (existing == value) {
          return;
        }
      }
      values.push_back(value);
    }

  } // namespace

  std::vector<uint64_t> drmAsyncScanoutModifiers(int drmFd, uint32_t format) {
    std::vector<uint64_t> modifiers;
    if (drmFd < 0) {
      return modifiers;
    }
    // Property blobs, including IN_FORMATS_ASYNC, are only visible with the
    // atomic client capability. wlroots already enables it on its own fd; this
    // makes the helper work for callers that open a fresh node too.
    drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModePlaneRes* planes = drmModeGetPlaneResources(drmFd);
    if (planes == nullptr) {
      return modifiers;
    }

    for (uint32_t i = 0; i < planes->count_planes; ++i) {
      const uint32_t planeId = planes->planes[i];
      drmModeObjectProperties* properties = drmModeObjectGetProperties(drmFd, planeId, DRM_MODE_OBJECT_PLANE);
      if (properties == nullptr) {
        continue;
      }

      uint64_t planeType = 0;
      const bool isPrimary =
          findProperty(drmFd, properties, "type", nullptr, &planeType) && planeType == DRM_PLANE_TYPE_PRIMARY;
      uint64_t asyncFormatsId = 0;
      const bool hasAsyncFormats =
          findProperty(drmFd, properties, "IN_FORMATS_ASYNC", nullptr, &asyncFormatsId) && asyncFormatsId != 0;
      drmModeFreeObjectProperties(properties);

      if (!isPrimary || !hasAsyncFormats) {
        continue;
      }

      drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(drmFd, static_cast<uint32_t>(asyncFormatsId));
      if (blob == nullptr) {
        continue;
      }
      drmModeFormatModifierIterator iterator{};
      while (drmModeFormatModifierBlobIterNext(blob, &iterator)) {
        if (iterator.fmt == format) {
          appendUnique(modifiers, iterator.mod);
        }
      }
      drmModeFreePropertyBlob(blob);
    }

    drmModeFreePlaneResources(planes);
    return modifiers;
  }

  std::vector<uint64_t>
  filterPreferredModifiers(const uint64_t* modifiers, std::size_t count, const std::vector<uint64_t>& preferred) {
    std::vector<uint64_t> filtered;
    if (modifiers == nullptr || count == 0 || preferred.empty()) {
      return filtered;
    }
    for (const uint64_t candidate : preferred) {
      for (std::size_t i = 0; i < count; ++i) {
        if (modifiers[i] == candidate) {
          appendUnique(filtered, candidate);
          break;
        }
      }
    }
    return filtered;
  }

} // namespace umbriel
