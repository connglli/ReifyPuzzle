#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace refractir {

  /**
   * Pointer provenance for a `ptr T` local: the base tag identifying the
   * addressed object and the size (in BV64 tag units) of its addressable
   * region (scalar = 1, array = N, struct = #fields).
   */
  struct PtrProvenance {
    std::uint64_t baseTag;
    std::uint64_t size;
  };

  /**
   * Per-solve provenance store: maps a `ptr T` local name to its
   * PtrProvenance. Updated on `%p = addr %x` / `ptrindex` / `ptrfield`;
   * pointer arithmetic preserves it; load-derived pointers have no entry
   * (their provenance is unknown). Reset at the start of each solve(); the
   * whole map is saved and restored around a nested callFunction so a callee
   * frame gets a clean provenance view (rule 14 / 19).
   *
   * The pointer-tag *encoding* (name → tag, type → tag-size) is stateless and
   * lives in solver/internal.hpp; this class owns only the mutable store.
   */
  class Provenance {
  public:
    using Map = std::unordered_map<std::string, PtrProvenance>;

    /// Provenance recorded for `name`, or nullopt if unknown.
    std::optional<PtrProvenance> lookup(const std::string &name) const {
      auto it = map_.find(name);
      if (it == map_.end())
        return std::nullopt;
      return it->second;
    }

    void set(const std::string &name, PtrProvenance e) { map_[name] = e; }

    void erase(const std::string &name) { map_.erase(name); }

    void clear() { map_.clear(); }

    /// Move the whole store out (leaving it empty) — for a call boundary.
    Map take() {
      Map m = std::move(map_);
      map_.clear(); // moved-from unordered_map is unspecified; force empty
      return m;
    }

    /// Move a previously taken store back in.
    void restore(Map saved) { map_ = std::move(saved); }

  private:
    Map map_;
  };

} // namespace refractir
