#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Measured seat-yaw convention: positive stick makes seat yaw DECREASE.
// This first test controls yaw only. Seat pitch is not a proven gun elevation
// measurement, so vertical input is a one-update stick pulse, with no integrator.
class Ao4TurretMouseControl {
 public:
  void Reset() { owner_ = 0; active_ = false; last_ = motion_at_ = 0; }
  bool Update(uint32_t owner, uint64_t now, float yaw, float dx, float dy,
              float scale, bool invert, float gain, float& x, float& y) {
    x = y = 0;
    if (!owner || !std::isfinite(yaw) || !std::isfinite(dx) || !std::isfinite(dy) ||
        !std::isfinite(scale) || scale < .00001f || scale > .05f ||
        !std::isfinite(gain) || gain <= 0 || gain >= 200 ||
        std::abs(dx) > 16384 || std::abs(dy) > 16384) { Reset(); return false; }
    if (owner != owner_ || now < last_ || now - last_ > 100) {
      Reset(); owner_ = owner; wanted_ = yaw;
    }
    last_ = now;
    if (dx != 0) {
      if (!active_) wanted_ = yaw;
      // Screen-right uses stick-right, hence a negative seat-yaw target delta.
      wanted_ -= dx * scale;
      motion_at_ = now;
      active_ = true;
    }
    // Never maintain a stale target indefinitely or start steering at entry.
    if (active_ && now - motion_at_ >= 250) active_ = false;
    if (active_) {
      constexpr float tau = 6.28318530718f;
      float error = std::remainder(wanted_ - yaw, tau);
      error = std::clamp(error, -.15f, .15f);
      wanted_ = yaw + error;
      if (std::abs(error) > .0003f) x = -Shape(error * gain);
    } else wanted_ = yaw;
    y = Shape(dy * scale * gain * (invert ? 1 : -1));
    return true;
  }
  float WantedYaw() const { return wanted_; }
 private:
  static float Shape(float value) {
    const float bounded = std::clamp(value, -1.0f, 1.0f);
    return std::copysign(std::sqrt(std::abs(bounded)), bounded);
  }
  uint32_t owner_ = 0;
  uint64_t last_ = 0, motion_at_ = 0;
  float wanted_ = 0;
  bool active_ = false;
};
