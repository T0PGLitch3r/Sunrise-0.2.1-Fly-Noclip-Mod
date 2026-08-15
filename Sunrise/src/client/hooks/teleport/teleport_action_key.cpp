/** Fly/noclip action-key resolution. */
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::hooks::teleport {
namespace {
constexpr std::string_view kScanSignatureText = "48 89 5C 24 18 48 89 6C 24 20 48 89 4C 24 08 56 57 41 55 41 56 41 57 48 83 EC 20 44 0F B6 3D ? ? ? ?";
constexpr auto kScanSignature = signature<signature_length(kScanSignatureText)>(kScanSignatureText);
constexpr std::array<std::byte, 5> kVirtualKeyLoad{std::byte{0x41}, std::byte{0x0F}, std::byte{0xB6}, std::byte{0xBC}, std::byte{0x06}};
constexpr std::array<std::byte, 5> kScanCodeLoad{std::byte{0x41}, std::byte{0x0F}, std::byte{0xB6}, std::byte{0x8C}, std::byte{0x06}};
constexpr std::size_t kSearchBytes = 0x140;
constexpr std::size_t kKeyTableCount = 105;
constexpr std::uint8_t kAbsentVirtualKey = 0xFF;
const std::uint8_t* g_virtualKeys{};
const std::uint8_t* g_scanCodes{};
[[nodiscard]] const std::uint8_t* table_from(std::byte* scan, const std::array<std::byte, 5>& opcode) noexcept {
    auto* const base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (base == nullptr) return nullptr;
    for (std::size_t offset = 0; offset + opcode.size() + sizeof(std::uint32_t) < kSearchBytes; ++offset) {
        std::byte* const site = scan + offset;
        if (std::memcmp(site, opcode.data(), opcode.size()) != 0) continue;
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, site + opcode.size(), sizeof displacement);
        return reinterpret_cast<const std::uint8_t*>(base + displacement);
    }
    return nullptr;
}
}
bool resolve_action_keys() noexcept {
    std::byte* const scan = scan_main_image_unique(kScanSignature, "teleport_key_scan");
    if (scan == nullptr) return false;
    g_virtualKeys = table_from(scan, kVirtualKeyLoad);
    g_scanCodes = table_from(scan, kScanCodeLoad);
    return g_virtualKeys != nullptr && g_scanCodes != nullptr;
}
void clear_action_keys() noexcept { g_virtualKeys = nullptr; g_scanCodes = nullptr; }
std::uint32_t action_key(std::uint16_t index) noexcept {
    if (g_virtualKeys == nullptr || g_scanCodes == nullptr || index >= kKeyTableCount) return 0;
    const std::uint8_t direct = g_virtualKeys[index];
    if (direct != kAbsentVirtualKey) return direct;
    return MapVirtualKeyExA(g_scanCodes[index], MAPVK_VSC_TO_VK, GetKeyboardLayout(0));
}
} // namespace sunrise::client::hooks::teleport
