#pragma once
#include <algorithm>
#include <cmath>

// First on-foot candidate. Conservative 85-degree pitch bound; original
// simulation still applies its own constraints. Zoom scaling is not calibrated.
inline bool Ao4MouseAngles(float yaw, float pitch, float dx, float dy,
                           double scale, bool invert_y, float& out_yaw,
                           float& out_pitch) {
  if (!std::isfinite(yaw) || !std::isfinite(pitch) ||
      !std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(scale) ||
      scale < 0.00001 || scale > 0.05 || std::abs(pitch) > 1.6 ||
      std::abs(dx) > 16384 || std::abs(dy) > 16384) return false;
  constexpr double tau = 6.283185307179586;
  // Positive screen-X is a right turn; screen-Y increases downwards.
  double next = std::fmod(double(yaw) + dx * scale, tau);
  if (next < 0) next += tau;
  out_yaw = float(next);
  out_pitch = float(std::clamp(double(pitch) + dy * scale * (invert_y ? 1 : -1),
                             -1.48352986419518, 1.48352986419518));
  return true;
}
