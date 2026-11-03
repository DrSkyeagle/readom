// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>

namespace pw3 {
struct Rect { int left, top, right, bottom; };
inline bool isEmpty(const Rect& r) { return r.right <= r.left || r.bottom <= r.top; }
inline Rect intersect(const Rect& a, const Rect& b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
// Smallest rectangle containing both; an empty one contributes nothing.
inline Rect bounds(const Rect& a, const Rect& b) {
    if (isEmpty(a)) return b;
    if (isEmpty(b)) return a;
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
// True also for an empty `inner`.
inline bool contains(const Rect& outer, const Rect& inner) {
    return isEmpty(inner) || (inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom);
}
inline long long area(const Rect& r) { return isEmpty(r) ? 0 : (long long)(r.right - r.left) * (r.bottom - r.top); }
}  // namespace pw3
