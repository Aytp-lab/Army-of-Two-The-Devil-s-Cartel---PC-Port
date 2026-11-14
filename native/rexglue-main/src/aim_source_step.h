#pragma once
#include <cmath>

// Bounded diagnostic angle step in radians; no attempt to define turret limits.
inline bool Ao4ChoosePitchStep(float current, float& target) {
  if (!std::isfinite(current) || current < -1.6f || current > 1.6f) return false;
  target = current + (current >= 1.3f ? -0.1f : 0.1f);
  return target >= -1.6f && target <= 1.6f;
}
