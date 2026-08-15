#pragma once
#include <cstdint>
namespace sunrise::client::teleport {
inline constexpr float kDefaultDistance = 10.0F;
inline constexpr float kDefaultFlySpeed = 0.75F;
inline constexpr float kMinimumFlySpeed = 0.05F;
inline constexpr float kMaximumFlySpeed = 10.0F;
inline constexpr float kMinimumDistance = 1.0F;
inline constexpr float kMaximumDistance = 100.0F;
inline constexpr std::uint32_t kNoKey = 0;
inline constexpr std::uint32_t kDefaultFlyKey = 0x75;
struct Settings { bool enabled{false}; float distance{kDefaultDistance}; float flySpeed{kDefaultFlySpeed}; std::uint32_t virtualKey{kNoKey}; std::uint32_t flyVirtualKey{kDefaultFlyKey}; };
void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Settings get() noexcept;
bool publish(const Settings& settings) noexcept;
} // namespace sunrise::client::teleport
