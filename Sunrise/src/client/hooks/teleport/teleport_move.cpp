/**
 * The teleport itself. The camera hook publishes a forward vector and reads the bound key once a
 * frame. The physics hook applies the move before the sync it runs ahead of. Physics owns the
 * position, so writing the object placement would move the camera alone.
 */

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../teleport/teleport_settings_store.h"
#include "../polled_input/runtime.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {

constexpr std::uint32_t kRequestLifetimeFrames = 3;
constexpr std::uint32_t kForceAfterFrames = 1;
constexpr std::uint32_t kPressFrames = 2;
constexpr std::uint16_t kForwardAction = static_cast<std::uint16_t>(state::account::settings::bindings::Action::moveForward);
constexpr std::uint16_t kJumpAction = static_cast<std::uint16_t>(state::account::settings::bindings::Action::jump);
constexpr std::uint16_t kCrouchAction = static_cast<std::uint16_t>(state::account::settings::bindings::Action::toggleCrouch);

std::atomic_bool g_requested{false};
std::atomic_bool g_forwardValid{false};
std::atomic_bool g_keyDown{false};
std::atomic_uint32_t g_requestAge{0};
std::atomic_bool g_active{false};
std::atomic_bool g_flyEnabled{false};
std::atomic_bool g_flyKeyWasDown{false};
std::atomic<std::byte*> g_playerComponent{nullptr};
std::atomic_uint32_t g_pressFrames{0};
ControlledHandle g_controlledHandle{};
CameraSingleton g_cameraSingleton{};
std::array<float, kVectorLanes> g_forward{};

template <typename T> [[nodiscard]] bool read_at(const std::byte* address, T& value) noexcept {
    if (address == nullptr) return false;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE && read == sizeof value;
}

[[nodiscard]] bool write_vector(std::byte* address, const std::array<float, kVectorLanes>& value) noexcept {
    if (address == nullptr) return false;
    SIZE_T written = 0;
    const SIZE_T size = sizeof(float) * kVectorLanes;
    return WriteProcessMemory(GetCurrentProcess(), address, value.data(), size, &written) != FALSE && written == size;
}

[[nodiscard]] std::byte* body_of(std::byte* component) noexcept {
    std::byte* array = nullptr;
    std::int32_t index = 0;
    if (!read_at(component + kPhysicsComponentBodyArray, array) || !read_at(component + kPhysicsComponentBodyIndex, index) || array == nullptr || index < 0) return nullptr;
    std::byte* body = nullptr;
    const std::size_t offset = kBodyEntryStride * static_cast<std::size_t>(index) + kBodyPointer;
    return read_at(array + offset, body) ? body : nullptr;
}

void expire_request() noexcept {
    if (!g_requested.load(std::memory_order_acquire)) return;
    if (g_requestAge.fetch_add(1, std::memory_order_relaxed) + 1 >= kRequestLifetimeFrames) g_requested.store(false, std::memory_order_release);
}

[[nodiscard]] bool perform_move(std::byte* component) noexcept;
void report_skip(const char* reason) noexcept;

void begin_press() noexcept {
    const state::AccountState account = state::account_snapshot();
    const auto& binding = account.settings.keyBindings.values[kForwardAction];
    if (!binding.primary.has_value()) return;
    const std::uint32_t virtualKey = action_key(*binding.primary);
    if (virtualKey == 0) { report_skip("no_key"); return; }
    hooks::polled_input::hold_key(virtualKey);
    g_pressFrames.store(kPressFrames, std::memory_order_release);
}

void poll_fly_toggle() noexcept {
    const auto settings = sunrise::client::teleport::get();
    const std::uint32_t key = settings.flyVirtualKey == sunrise::client::teleport::kNoKey ? sunrise::client::teleport::kDefaultFlyKey : settings.flyVirtualKey;
    const bool down = key <= 254 && (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    const bool wasDown = g_flyKeyWasDown.exchange(down, std::memory_order_acq_rel);
    if (down && !wasDown) {
        const bool enabled = !g_flyEnabled.load(std::memory_order_acquire);
        g_flyEnabled.store(enabled, std::memory_order_release);
        core::log::write(core::log::Channel::client, core::log::Level::info, enabled ? "ev=fly mode=enabled" : "ev=fly mode=disabled");
    }
}

void end_press() noexcept {
    if (g_pressFrames.load(std::memory_order_acquire) == 0) return;
    if (g_pressFrames.fetch_sub(1, std::memory_order_acq_rel) <= 1) hooks::polled_input::release_key();
}

void report_gates(const std::byte* component, const std::byte* body) noexcept {
    std::uint8_t suppressed = 0;
    std::int32_t bodyIndex = 0;
    std::uint32_t bodyFlags = 0;
    std::uint8_t motionType = 0;
    (void)read_at(component + kPhysicsComponentSuppress, suppressed);
    (void)read_at(component + kPhysicsComponentBodyIndex, bodyIndex);
    (void)read_at(body + kBodyFlags, bodyFlags);
    (void)read_at(body + kBodyMotionType, motionType);
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(), line.size(), "ev=teleport stage=gates suppress=%u index=%d flags=0x%08X active=%u motion=%u", static_cast<unsigned>(suppressed), static_cast<int>(bodyIndex), static_cast<unsigned>(bodyFlags), (bodyFlags & kBodyActiveBit) != 0 ? 1U : 0U, static_cast<unsigned>(motionType));
    if (written > 0) core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), static_cast<std::size_t>(written)});
}

[[nodiscard]] bool owns_player(std::byte* component) noexcept {
    std::uint32_t controlled = kInvalidHandle;
    g_controlledHandle(&controlled);
    if (controlled == kInvalidHandle) return false;
    std::uint16_t owner = 0;
    return read_at(component + kPhysicsComponentObjectHandle, owner) && (controlled & kHandleIndexMask) == (static_cast<std::uint32_t>(owner) & kHandleIndexMask);
}

void report_skip(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(), line.size(), "ev=teleport stage=move result=skip reason=%s", reason);
    if (written > 0) core::log::write(core::log::Channel::client, core::log::Level::warn, {line.data(), static_cast<std::size_t>(written)});
}

void set_vertical_velocity(std::byte* body, float value) noexcept {
    std::array<float, kVectorLanes> velocity{};
    if (!read_at(body + kBodyVelocityX, velocity)) return;
    velocity[kVerticalLane] = value;
    (void)write_vector(body + kBodyVelocityX, velocity);
}

[[nodiscard]] bool offset_vector(std::byte* address, const std::array<float, kVectorLanes>& delta, std::array<float, kVectorLanes>& before, std::array<float, kVectorLanes>& after) noexcept {
    if (!read_at(address, before)) return false;
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) after[lane] = before[lane] + delta[lane];
    return write_vector(address, after);
}

[[nodiscard]] bool move_body(std::byte* body, float distance) noexcept {
    std::array<float, kVectorLanes> delta{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) delta[lane] = g_forward[lane] * distance;
    std::array<float, kVectorLanes> position{};
    std::array<float, kVectorLanes> moved{};
    if (!offset_vector(body + kBodyPositionX, delta, position, moved)) { report_skip("body"); return false; }
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(), line.size(), "ev=teleport stage=move result=ok dist=%.1f from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f", static_cast<double>(distance), static_cast<double>(position[0]), static_cast<double>(position[1]), static_cast<double>(position[2]), static_cast<double>(moved[0]), static_cast<double>(moved[1]), static_cast<double>(moved[2]));
    if (written > 0) core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), static_cast<std::size_t>(written)});
    return true;
}

[[nodiscard]] std::uint32_t bound_key(std::uint16_t actionIndex, std::uint32_t fallback) noexcept {
    const state::AccountState account = state::account_snapshot();
    const auto& binding = account.settings.keyBindings.values[actionIndex];
    if (binding.primary.has_value()) {
        const std::uint32_t resolved = action_key(*binding.primary);
        if (resolved != 0) return resolved;
    }
    return fallback;
}

bool apply_fly(std::byte* component) noexcept {
    if (!g_flyEnabled.load(std::memory_order_acquire) || !owns_player(component)) return false;
    std::byte* const body = body_of(component);
    if (body == nullptr) return false;
    const bool forward = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool backward = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool left = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool right = (GetAsyncKeyState('D') & 0x8000) != 0;
    const std::uint32_t jumpKey = bound_key(kJumpAction, VK_SPACE);
    const std::uint32_t crouchKey = bound_key(kCrouchAction, VK_CONTROL);
    const bool up = (GetAsyncKeyState(static_cast<int>(jumpKey)) & 0x8000) != 0;
    const bool down = (GetAsyncKeyState(static_cast<int>(crouchKey)) & 0x8000) != 0;
    std::array<float, kVectorLanes> delta{};
    const float forwardAxis = static_cast<float>(forward) - static_cast<float>(backward);
    const float strafeAxis = static_cast<float>(right) - static_cast<float>(left);
    const float verticalAxis = static_cast<float>(up) - static_cast<float>(down);
    const float fx = g_forward[0];
    const float fy = g_forward[1];
    const float fz = g_forward[2];
    const float forwardLength = std::sqrt(fx * fx + fy * fy + fz * fz);
    const float nx = forwardLength > 0.001F ? fx / forwardLength : 1.0F;
    const float ny = forwardLength > 0.001F ? fy / forwardLength : 0.0F;
    const float nz = forwardLength > 0.001F ? fz / forwardLength : 0.0F;
    const float rightLength = std::sqrt(ny * ny + nx * nx);
    const float rx = rightLength > 0.001F ? ny / rightLength : 0.0F;
    const float ry = rightLength > 0.001F ? -nx / rightLength : 1.0F;
    const float speed = client::teleport::get().flySpeed;
    delta[0] = (nx * forwardAxis + rx * strafeAxis) * speed;
    delta[1] = (ny * forwardAxis + ry * strafeAxis) * speed;
    delta[kVerticalLane] = (nz * forwardAxis + verticalAxis) * speed;
    std::array<float, kVectorLanes> position{};
    std::array<float, kVectorLanes> moved{};
    if (!offset_vector(body + kBodyPositionX, delta, position, moved)) return false;
    set_vertical_velocity(body, 0.0F);
    return true;
}

[[nodiscard]] bool perform_move(std::byte* component) noexcept {
    std::byte* const body = body_of(component);
    if (body == nullptr) { report_skip("no_body"); return false; }
    report_gates(component, body);
    set_vertical_velocity(body, 0.0F);
    if (!move_body(body, client::teleport::get().distance)) return false;
    begin_press();
    return true;
}

} // namespace

void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept { g_controlledHandle = controlled; g_cameraSingleton = singleton; }

void clear_targets() noexcept {
    g_controlledHandle = nullptr;
    g_cameraSingleton = nullptr;
    g_requested.store(false, std::memory_order_release);
    g_forwardValid.store(false, std::memory_order_release);
    g_keyDown.store(false, std::memory_order_relaxed);
    g_requestAge.store(0, std::memory_order_relaxed);
    g_active.store(false, std::memory_order_relaxed);
    g_flyEnabled.store(false, std::memory_order_relaxed);
    g_flyKeyWasDown.store(false, std::memory_order_relaxed);
    g_playerComponent.store(nullptr, std::memory_order_relaxed);
}

void capture_forward(std::uint32_t playerIndex) noexcept {
    if (playerIndex == kInvalidHandle || g_cameraSingleton == nullptr) return;
    std::byte* const camera = g_cameraSingleton();
    if (camera == nullptr) return;
    std::array<float, kVectorLanes> forward{};
    if (!read_at(camera + kCameraBlockStride * playerIndex + kCameraForwardX, forward)) return;
    g_forward = forward;
    g_forwardValid.store(true, std::memory_order_release);
}

void poll_request() noexcept {
    poll_fly_toggle();
    end_press();
    expire_request();
    const client::teleport::Settings settings = client::teleport::get();
    const bool usable = settings.enabled && settings.virtualKey != client::teleport::kNoKey;
    g_active.store(usable, std::memory_order_relaxed);
    if (!usable) { g_keyDown.store(false, std::memory_order_relaxed); return; }
    if (core::ui::runtime::snapshot().visible) { g_keyDown.store(false, std::memory_order_relaxed); return; }
    const bool down = (GetAsyncKeyState(static_cast<int>(settings.virtualKey)) & 0x8000) != 0;
    if (down && !g_keyDown.exchange(down, std::memory_order_relaxed)) {
        g_requestAge.store(0, std::memory_order_relaxed);
        g_requested.store(true, std::memory_order_release);
        return;
    }
    g_keyDown.store(down, std::memory_order_relaxed);
}

void apply_pending(void* component) noexcept {
    poll_fly_toggle();
    if (component == nullptr || g_controlledHandle == nullptr) return;
    std::byte* const physics = static_cast<std::byte*>(component);
    const bool flyEnabled = g_flyEnabled.load(std::memory_order_acquire);
    const bool requested = g_requested.load(std::memory_order_acquire);
    if (flyEnabled) {
        if (owns_player(physics)) g_playerComponent.store(physics, std::memory_order_relaxed);
        return;
    }
    if (!g_active.load(std::memory_order_relaxed) || !requested) return;
    if (!owns_player(physics)) return;
    g_playerComponent.store(physics, std::memory_order_relaxed);
    if (!g_forwardValid.load(std::memory_order_acquire)) return;
    g_requested.store(false, std::memory_order_release);
    (void)perform_move(physics);
}

bool apply_fly_post_sync(void* component) noexcept {
    if (!g_flyEnabled.load(std::memory_order_acquire) || component == nullptr) return false;
    std::byte* const physics = static_cast<std::byte*>(component);
    if (!owns_player(physics)) return false;
    g_playerComponent.store(physics, std::memory_order_relaxed);
    return apply_fly(physics);
}

void force_pending() noexcept {
    if (!g_requested.load(std::memory_order_acquire) || !g_forwardValid.load(std::memory_order_acquire) || g_requestAge.load(std::memory_order_relaxed) < kForceAfterFrames) return;
    std::byte* const physics = g_playerComponent.load(std::memory_order_relaxed);
    if (physics == nullptr || g_controlledHandle == nullptr || !owns_player(physics)) return;
    g_requested.store(false, std::memory_order_release);
    if (!perform_move(physics)) return;
    invoke_sync(physics);
    core::log::write(core::log::Channel::client, core::log::Level::info, "ev=teleport stage=force result=ok");
}

} // namespace sunrise::client::hooks::teleport
