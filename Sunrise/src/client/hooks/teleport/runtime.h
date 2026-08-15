#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::teleport {

using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
using CameraSingleton = std::byte* (*)();

void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;
void clear_targets() noexcept;
[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;
void capture_forward(std::uint32_t playerIndex) noexcept;
void poll_request() noexcept;
void force_pending() noexcept;
[[nodiscard]] bool resolve_action_keys() noexcept;
void clear_action_keys() noexcept;
[[nodiscard]] std::uint32_t action_key(std::uint16_t index) noexcept;
void invoke_sync(void* component) noexcept;
void apply_pending(void* component) noexcept;
[[nodiscard]] bool apply_fly_post_sync(void* component) noexcept;

} // namespace sunrise::client::hooks::teleport
