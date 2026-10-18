#pragma once

#include <Arduino.h>

// 466x466 embedded translation of gemini-code-1791701389511.html (v12).
// Coordinates from the 320x320 mock-up are scaled by 466 / 320 = 1.45625.
namespace UiV12 {
constexpr uint16_t WIDTH = 466;
constexpr uint16_t HEIGHT = 466;
constexpr int16_t CENTER = 233;
constexpr float MOCKUP_SCALE = 1.45625f;

// RGB888 palette. Display adapters convert these to the panel's color format.
constexpr uint32_t BLACK = 0x000000;
constexpr uint32_t SURFACE = 0x080808;
constexpr uint32_t FIBER_GRAY = 0x646464;
constexpr uint32_t FIBER_BLUE = 0x2F5BFF;
constexpr uint32_t CYAN = 0x00E5FF;
constexpr uint32_t PINK = 0xFF0080;
constexpr uint32_t GREEN = 0x32FF7E;
constexpr uint32_t RED = 0xFF4757;
constexpr uint32_t GOLD = 0xF1C40F;
constexpr uint32_t WHITE = 0xFFFFFF;

constexpr uint16_t BOOT_TOTAL_MS = 1200;
// Shortened from the browser mock-up's 350 ms because a 466x466 MCU
// framebuffer feels more responsive with a compact transition.
constexpr uint16_t PAGE_TRANSITION_MS = 210;
constexpr uint8_t ICON_STROKE_PX = 3;       // 2.2 logical px, scaled to 466
constexpr uint8_t GHOST_FIBER_COUNT = 8;    // 4 gray + 4 blue
constexpr int16_t GHOST_FIBER_ANGLE_DEG = -58;
constexpr uint8_t DOT_SPACING = 26;         // 18 logical px, scaled
constexpr uint8_t DOT_RADIUS = 2;
constexpr uint16_t DOT_BULGE_RADIUS = 146;  // 100 logical px, scaled
constexpr uint8_t DOT_BULGE_STRENGTH = 44;

constexpr uint16_t NAV_CENTER_RADIUS = 61;  // 84 logical px diameter
constexpr uint16_t MEDIA_DIAL_RADIUS = 198; // 136 logical px
constexpr uint16_t MEDIA_DIAL_SWEEP_DEG = 260;
constexpr uint8_t MEDIA_DIAL_WIDTH = 9;
constexpr uint16_t SYSTEM_DIAMOND_SIZE = 99;

struct AnimationTuning {
  float ghostFiberPhaseStep = 0.015f;
  float ghostFiberRotationStep = 0.03f;
  float volumeFollowTau = 0.08f;
  float volumeVelocityTau = 0.06f;
  uint8_t cometTailSegments = 12;
  uint16_t cometReachDeg = 180;
};

constexpr AnimationTuning ANIMATION{};
} // namespace UiV12
