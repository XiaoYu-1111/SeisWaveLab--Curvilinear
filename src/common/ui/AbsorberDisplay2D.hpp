#pragma once
#include "imgui.h"
#include <algorithm>

namespace SeisWave::Ui {
// Preserve the original amber boundary hue while leaving the physical wavefield visible.
inline constexpr float defaultAbsorberOpacity2D = .14f;
inline ImVec4 tintAbsorber2D(ImVec4 field, float opacity) {
    const float alpha = std::clamp(opacity, 0.f, 1.f);
    return {field.x * (1 - alpha) + (237.f / 255) * alpha, field.y * (1 - alpha) + (139.f / 255) * alpha,
            field.z * (1 - alpha) + (35.f / 255) * alpha, field.w};
}
} // namespace SeisWave::Ui
