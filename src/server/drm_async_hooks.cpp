// wlroots always includes every plane it knows about in the atomic request,
// even when a plane is already disabled: atomic_connector_add() calls
// plane_disable() for the cursor plane, and plane_disable() emits FB_ID=0 and
// CRTC_ID=0 unconditionally.
//
// Intel's atomic async-flip validation rejects the whole request if any plane
// other than the primary is present in the atomic state, and also rejects a
// plane whose old or new framebuffer is NULL. An unchanged "disable the
// already-disabled cursor plane" item therefore makes every async flip fail.
//
// Strip a non-primary plane's items when every value in the request already
// matches the plane's current state. An item group that actually changes a
// plane is left untouched, so wlroots sees the kernel's error and falls back
// to a regular commit for that frame.

#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <map>
#include <string_view>
#include <vector>

extern "C" {
#include <xf86drmMode.h>
}

namespace {

  struct AtomicReqItem {
    uint32_t objectId;
    uint32_t propertyId;
    uint64_t value;
    uint32_t cursor;
  };

  struct AtomicReqLayout {
    uint32_t cursor;
    uint32_t sizeItems;
    AtomicReqItem* items;
  };

  static_assert(sizeof(AtomicReqLayout) == 16);
  // libdrm >= 2.4.129 tracks a per-item cursor after `value`; the trailing
  // padding brings the stride to 24 bytes. The project's libdrm floor is
  // 2.4.129, so this layout is stable across supported versions.
  static_assert(sizeof(AtomicReqItem) == 24);

  using AtomicCommitFn = int (*)(int, drmModeAtomicReqPtr, uint32_t, void*);

  AtomicCommitFn realAtomicCommit() {
    static AtomicCommitFn function = reinterpret_cast<AtomicCommitFn>(dlsym(RTLD_NEXT, "drmModeAtomicCommit"));
    return function;
  }

  struct PlaneInfo {
    uint32_t id = 0;
    bool primary = false;
  };

  std::vector<PlaneInfo> primaryPlaneInfo(int fd) {
    std::vector<PlaneInfo> result;
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    if (planes == nullptr) {
      return result;
    }
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
      const uint32_t planeId = planes->planes[i];
      drmModeObjectProperties* properties = drmModeObjectGetProperties(fd, planeId, DRM_MODE_OBJECT_PLANE);
      if (properties == nullptr) {
        continue;
      }
      bool primary = false;
      for (uint32_t p = 0; p < properties->count_props && !primary; ++p) {
        drmModePropertyRes* property = drmModeGetProperty(fd, properties->props[p]);
        if (property == nullptr) {
          continue;
        }
        primary = std::string_view(property->name) == "type" && properties->prop_values[p] == DRM_PLANE_TYPE_PRIMARY;
        drmModeFreeProperty(property);
      }
      drmModeFreeObjectProperties(properties);
      result.push_back(PlaneInfo{.id = planeId, .primary = primary});
    }
    drmModeFreePlaneResources(planes);
    return result;
  }

  // Plane IDs and types are fixed for the lifetime of a DRM device. Cache the
  // per-fd scan so an async flip does not perform one ioctl per property on
  // every frame.
  const std::vector<PlaneInfo>& cachedPlaneInfo(int fd) {
    static std::map<int, std::vector<PlaneInfo>> cache;
    auto [it, inserted] = cache.try_emplace(fd);
    if (inserted) {
      it->second = primaryPlaneInfo(fd);
    }
    return it->second;
  }

  // True when the plane is currently inactive and every property in `items`
  // already has the requested value. Returns false if the plane state cannot be
  // read: leaving the items in place is always the safe choice.
  bool planeIsUnchanged(int fd, uint32_t planeId, const std::vector<AtomicReqItem>& items) {
    drmModePlane* plane = drmModeGetPlane(fd, planeId);
    if (plane == nullptr) {
      return false;
    }
    const bool inactive = plane->fb_id == 0 && plane->crtc_id == 0;
    drmModeFreePlane(plane);
    if (!inactive) {
      return false;
    }

    drmModeObjectProperties* properties = drmModeObjectGetProperties(fd, planeId, DRM_MODE_OBJECT_PLANE);
    if (properties == nullptr) {
      return false;
    }
    bool unchanged = true;
    for (const AtomicReqItem& item : items) {
      bool found = false;
      for (uint32_t i = 0; i < properties->count_props; ++i) {
        if (properties->props[i] == item.propertyId) {
          found = true;
          if (properties->prop_values[i] != item.value) {
            unchanged = false;
          }
          break;
        }
      }
      if (!found) {
        unchanged = false;
      }
      if (!unchanged) {
        break;
      }
    }
    drmModeFreeObjectProperties(properties);
    return unchanged;
  }

} // namespace

extern "C" int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr request, uint32_t flags, void* userData) {
  const AtomicCommitFn commit = realAtomicCommit();
  if (commit == nullptr) {
    return -1;
  }
  if (request == nullptr || (flags & DRM_MODE_PAGE_FLIP_ASYNC) == 0) {
    return commit(fd, request, flags, userData);
  }

  const int count = drmModeAtomicGetCursor(request);
  if (count <= 0) {
    return commit(fd, request, flags, userData);
  }
  const auto* layout = reinterpret_cast<const AtomicReqLayout*>(request);
  if (layout->items == nullptr) {
    return commit(fd, request, flags, userData);
  }

  std::vector<AtomicReqItem> items(layout->items, layout->items + static_cast<std::size_t>(count));

  const std::vector<PlaneInfo>& planes = cachedPlaneInfo(fd);
  if (planes.empty()) {
    return commit(fd, request, flags, userData);
  }

  std::vector<uint32_t> stripped;
  for (const PlaneInfo& plane : planes) {
    if (plane.primary) {
      continue;
    }
    std::vector<AtomicReqItem> planeItems;
    for (const AtomicReqItem& item : items) {
      if (item.objectId == plane.id) {
        planeItems.push_back(item);
      }
    }
    if (!planeItems.empty() && planeIsUnchanged(fd, plane.id, planeItems)) {
      stripped.push_back(plane.id);
    }
  }
  if (stripped.empty()) {
    return commit(fd, request, flags, userData);
  }

  drmModeAtomicReqPtr filtered = drmModeAtomicAlloc();
  if (filtered == nullptr) {
    return commit(fd, request, flags, userData);
  }
  for (const AtomicReqItem& item : items) {
    bool drop = false;
    for (const uint32_t objectId : stripped) {
      if (item.objectId == objectId) {
        drop = true;
        break;
      }
    }
    if (drop) {
      continue;
    }
    if (drmModeAtomicAddProperty(filtered, item.objectId, item.propertyId, item.value) < 0) {
      drmModeAtomicFree(filtered);
      return commit(fd, request, flags, userData);
    }
  }

  const int result = commit(fd, filtered, flags, userData);
  drmModeAtomicFree(filtered);
  return result;
}
