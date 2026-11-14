#pragma once
#include <algorithm>
#include <cmath>

// Temporary bounded stick mapping, not a native turret angle setter. No seat
// angle feedback, stored target, square-root amplification, or automatic chase.
inline bool Ao4TurretMousePulse(float dx, float dy, double horizontal, double vertical, bool invert,
                                float& x, float& y) {
  x = y = 0;
  if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(horizontal) ||
      !std::isfinite(vertical) || horizontal < .01 || horizontal > 10 ||
      vertical < .01 || vertical > 10 || std::abs(dx) > 16384 || std::abs(dy) > 16384)
    return false;
  const double raw_x = dx * .016 * horizontal;
  const double raw_y = dy * .016 * vertical * (invert ? 1 : -1);
  const double divisor = std::max({1.0, std::abs(raw_x), std::abs(raw_y)});
  x = float(raw_x / divisor);
  y = float(raw_y / divisor);
  return true;
}
