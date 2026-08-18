#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../activity/defaults/activity_defaults_validation.h"
#include "../build_data/runtime.h"
#include "equipment/configured_equipment_identity.h"
#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::storage {

State g_state;
SRWLOCK g_stateLock{SRWLOCK_INIT};

} // namespace runtime::storage

namespace {

/** Module handle used to locate the local persistent loadout file. */
HMODULE g_module = nullptr;
constexpr std::uint32_t kSavedLoadoutMagic = 0x53554E4CU; // "SUNL"
constexpr std::uint32_t kSavedLoadoutVersion = 2U;
constexpr wchar_t kSavedLoadoutName[] = L"saved_loadout.bin";

[[nodiscard]] bool saved_loadout_path(wchar_t (&output)[MAX_PATH]) noexcept {
    if (g_module == nullptr) return false;
    const DWORD length = GetModuleFileNameW(g_module, output, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    std::size_t end = length;
    while (end > 0 && output[end - 1] != L'\\' && output[end - 1] != L'/') --end;
    if (end == 0 || end + (sizeof(kSavedLoadoutName) / sizeof(wchar_t)) >= MAX_PATH) return false;
    constexpr std::size_t nameCount = sizeof(kSavedLoadoutName) / sizeof(wchar_t);
    std::copy(kSavedLoadoutName, kSavedLoadoutName + nameCount, output + end);
    return true;
}

template <typename T>
[[nodiscard]] bool write_exact(HANDLE file, const T& value) noexcept {
    DWORD written = 0;
    return WriteFile(file, &value, static_cast<DWORD>(sizeof(T)), &written, nullptr) != FALSE
           && written == sizeof(T);
}

template <typename T>
[[nodiscard]] bool read_exact(HANDLE file, T& value) noexcept {
    DWORD read = 0;
    return ReadFile(file, &value, static_cast<DWORD>(sizeof(T)), &read, nullptr) != FALSE
           && read == sizeof(T);
}

/** Network-order IPv4 loopback returned by the in-process SignOn route. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** Default one-hour lifetime for generated SignOn session tokens. */
constexpr std::uint32_t kDefaultTokenLifetimeSeconds = 3600;
/** Family 5 uses the largest signed 64-bit value as its process-global object key. */
constexpr std::uint64_t kGlobalFamily5Soid =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/**
 * Fills fixed secret storage with Windows system randomness.
 * @tparam Size Required secret byte count.
 * @param output Secret storage to overwrite.
 * @return True when Windows generates every byte.
 */
template <std::size_t Size>
[[nodiscard]] bool randomize(std::array<std::byte, Size>& output) noexcept {
    return BCryptGenRandom(nullptr,
                           reinterpret_cast<PUCHAR>(output.data()),
                           static_cast<ULONG>(output.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG)
           >= 0;
}

/** @return True when any authored or already-seeded account identity owns one SOID. */
[[nodiscard]] bool identity_uses_soid(const AccountState& accountState,
                                      std::uint64_t soid) noexcept {
    if (soid == 0 || accountState.primarySoid == soid) {
        return true;
    }
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        if (accountState.profileItems[index].instanceSoid == soid) {
            return true;
        }
    }
    for (std::size_t characterIndex = 0; characterIndex < accountState.characterCount;
         ++characterIndex) {
        const CharacterState& character = accountState.characters[characterIndex];
        if (character.soid == soid) {
            return true;
        }
        for (const std::optional<account::inventory::Item>& item : character.equipment.slots) {
            if (item.has_value() && item->instanceSoid == soid) {
                return true;
            }
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            if (character.inventory.values[index].instanceSoid == soid) {
                return true;
            }
        }
    }
    return false;
}

/** Seeds canonical character row generations before installed build data is needed. */
[[nodiscard]] bool seed_inventory_runtime_fields(AccountState& accountState) noexcept {
    if (!account::valid_authored(accountState)) {
        return false;
    }
    for (std::size_t characterIndex = 0; characterIndex < accountState.characterCount;
         ++characterIndex) {
        CharacterState& character = accountState.characters[characterIndex];
        std::uint32_t next = 0;
        for (std::optional<account::inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()) {
                item->mutationSerial = static_cast<std::int32_t>(next++);
            }
        }
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            character.inventory.values[index].mutationSerial = static_cast<std::int32_t>(next++);
        }
        character.nextInventorySerial = next;
    }
    return account::valid(accountState);
}

/**
 * Canonicalizes only profile rows which the installed socket UI materializes as action sources.
 * @param accountState Account canonicalized in place.
 * @return True when every profile row canonicalizes.
 */
[[nodiscard]] bool canonicalize_profile_item_identities(AccountState& accountState) noexcept {
    if (!account::valid(accountState) || !build_data::socket_plug_rules_ready()) {
        return false;
    }
    std::array<bool, account::inventory::kProfileItemCapacity> actionSources{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const account::inventory::ProfileItem& profileItem = accountState.profileItems[index];
        build_data::items::Definition item{};
        build_data::items::details::Definition detail{};
        build_data::inventory::buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_hash(profileItem.definitionHash, item)
            || item.definitionHash != profileItem.definitionHash
            || !build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || detail.instancedDefinitionState
                   != build_data::items::details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile) {
            return false;
        }
        actionSources[index] =
            build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
        if (actionSources[index]
            && ++actionSourceCount > account::inventory::kProfileActionSourceCapacity) {
            return false;
        }
    }

    // Currency, material, and consumable rows are native non-instanced stacks.  Clear any stale
    // runtime key before allocating action-source identities so it cannot reserve the namespace.
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        if (!actionSources[index]) {
            accountState.profileItems[index].instanceSoid = 0;
        }
    }

    std::uint64_t nextProfileSoid = account::inventory::kFirstProfileItemInstanceSoid;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        account::inventory::ProfileItem& item = accountState.profileItems[index];
        if (!actionSources[index] || item.instanceSoid != 0) {
            continue;
        }
        while (identity_uses_soid(accountState, nextProfileSoid)) {
            if (nextProfileSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            ++nextProfileSoid;
        }
        item.instanceSoid = nextProfileSoid;
        if (nextProfileSoid != (std::numeric_limits<std::uint64_t>::max)()) {
            ++nextProfileSoid;
        }
    }
    return account::valid(accountState);
}

[[nodiscard]] bool persist_equipment_file(const AccountState& account) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!saved_loadout_path(path)) return false;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::uint32_t characterCount = static_cast<std::uint32_t>(
        (std::min)(account.characterCount, account.characters.size()));
    bool ok = write_exact(file, kSavedLoadoutMagic)
              && write_exact(file, kSavedLoadoutVersion)
              && write_exact(file, characterCount);
    for (std::size_t ci = 0; ok && ci < characterCount; ++ci) {
        const auto& character = account.characters[ci];
        ok = write_exact(file, static_cast<std::uint32_t>(character.equipment.slots.size()));
        for (const auto& optionalItem : character.equipment.slots) {
            const std::uint8_t present = optionalItem.has_value() ? 1U : 0U;
            ok = ok && write_exact(file, present);
            if (!ok || !present) continue;
            const auto& item = *optionalItem;
            ok = write_exact(file, item.instanceSoid) && write_exact(file, item.definitionHash)
                 && write_exact(file, item.level) && write_exact(file, item.quantity)
                 && write_exact(file, item.mutationSerial) && write_exact(file, item.flags)
                 && write_exact(file, item.sockets.policy)
                 && write_exact(file, static_cast<std::uint32_t>(item.sockets.plugCount));
            for (const auto& plug : item.sockets.plugs) {
                const std::uint8_t hasPlug = plug.has_value() ? 1U : 0U;
                ok = ok && write_exact(file, hasPlug);
                if (hasPlug) ok = ok && write_exact(file, *plug);
            }
        }
        ok = ok && write_exact(file, static_cast<std::uint32_t>(character.inventory.count));
        for (std::size_t ii = 0; ok && ii < character.inventory.count; ++ii) {
            const auto& item = character.inventory.values[ii];
            ok = write_exact(file, item.instanceSoid) && write_exact(file, item.definitionHash)
                 && write_exact(file, item.level) && write_exact(file, item.quantity)
                 && write_exact(file, item.mutationSerial) && write_exact(file, item.flags)
                 && write_exact(file, item.sockets.policy)
                 && write_exact(file, static_cast<std::uint32_t>(item.sockets.plugCount));
            for (const auto& plug : item.sockets.plugs) {
                const std::uint8_t hasPlug = plug.has_value() ? 1U : 0U;
                ok = ok && write_exact(file, hasPlug);
                if (hasPlug) ok = ok && write_exact(file, *plug);
            }
        }
    }
    const bool closed = CloseHandle(file) != FALSE;
    if (!ok || !closed) (void)DeleteFileW(path);
    return ok && closed;
}

[[nodiscard]] bool restore_equipment_file(AccountState& account) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!saved_loadout_path(path)) return true;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    std::uint32_t magic = 0, version = 0, characterCount = 0;
    bool ok = read_exact(file, magic) && read_exact(file, version) && read_exact(file, characterCount)
              && magic == kSavedLoadoutMagic && (version == 1U || version == kSavedLoadoutVersion)
              && characterCount <= account.characters.size();
    AccountState candidate = account;
    for (std::size_t ci = 0; ok && ci < characterCount; ++ci) {
        std::uint32_t slotCount = 0;
        ok = read_exact(file, slotCount) && slotCount == candidate.characters[ci].equipment.slots.size();
        auto& slots = candidate.characters[ci].equipment.slots;
        for (std::size_t si = 0; ok && si < slots.size(); ++si) {
            std::uint8_t present = 0;
            ok = read_exact(file, present) && present <= 1U;
            if (!ok) break;
            if (!present) { slots[si].reset(); continue; }
            account::inventory::Item item{}; std::uint32_t plugCount = 0;
            ok = read_exact(file, item.instanceSoid) && read_exact(file, item.definitionHash)
                 && read_exact(file, item.level) && read_exact(file, item.quantity)
                 && read_exact(file, item.mutationSerial) && read_exact(file, item.flags)
                 && read_exact(file, item.sockets.policy) && read_exact(file, plugCount)
                 && plugCount <= item.sockets.plugs.size();
            item.sockets.plugCount = plugCount;
            for (std::size_t pi = 0; ok && pi < item.sockets.plugs.size(); ++pi) {
                std::uint8_t hasPlug = 0; ok = read_exact(file, hasPlug) && hasPlug <= 1U;
                if (ok && hasPlug) { std::uint32_t plug = 0; ok = read_exact(file, plug); item.sockets.plugs[pi] = plug; }
                else item.sockets.plugs[pi].reset();
            }
            if (ok) slots[si] = item;
        }
        if (ok && version >= 2U) {
            std::uint32_t inventoryCount = 0;
            ok = read_exact(file, inventoryCount) && inventoryCount <= candidate.characters[ci].inventory.values.size();
            auto& inventory = candidate.characters[ci].inventory;
            if (ok) {
                inventory.count = inventoryCount;
                for (std::size_t ii = 0; ok && ii < inventory.count; ++ii) {
                    auto& item = inventory.values[ii]; item = {}; std::uint32_t plugCount = 0;
                    ok = read_exact(file, item.instanceSoid) && read_exact(file, item.definitionHash)
                         && read_exact(file, item.level) && read_exact(file, item.quantity)
                         && read_exact(file, item.mutationSerial) && read_exact(file, item.flags)
                         && read_exact(file, item.sockets.policy) && read_exact(file, plugCount)
                         && plugCount <= item.sockets.plugs.size();
                    item.sockets.plugCount = plugCount;
                    for (std::size_t pi = 0; ok && pi < item.sockets.plugs.size(); ++pi) {
                        std::uint8_t hasPlug = 0; ok = read_exact(file, hasPlug) && hasPlug <= 1U;
                        if (ok && hasPlug) { std::uint32_t plug = 0; ok = read_exact(file, plug); item.sockets.plugs[pi] = plug; }
                        else item.sockets.plugs[pi].reset();
                    }
                }
            }
        }
    }
    CloseHandle(file);
    if (!ok || !account::valid_authored(candidate)) return false;
    for (std::size_t ci = 0; ci < characterCount; ++ci) {
        auto& character = candidate.characters[ci];
        for (std::size_t ii = 0; ii < character.inventory.count;) {
            const auto soid = character.inventory.values[ii].instanceSoid;
            bool equipped = false;
            for (const auto& e : character.equipment.slots)
                if (e.has_value() && e->instanceSoid == soid) { equipped = true; break; }
            if (!equipped) { ++ii; continue; }
            for (std::size_t move = ii; move + 1U < character.inventory.count; ++move)
                character.inventory.values[move] = character.inventory.values[move + 1U];
            --character.inventory.count; character.inventory.values[character.inventory.count] = {};
        }
    }
    if (!account::valid_authored(candidate)) return false;
    account = candidate;
    return true;
}

} // namespace

/** Persists the current authored equipment and character inventory. */
bool persist_saved_loadout() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState account = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return persist_equipment_file(account);
}

/** Restores authored equipment and character inventory from saved_loadout.bin. */
bool restore_saved_loadout(AccountState& account) noexcept {
    return restore_equipment_file(account);
}

/**
 * Loads build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @return True when the cached data passes its checks and every secret gets random bytes.
 */
bool initialize(void* module, const AccountState& initialAccount) noexcept {
    return initialize(module, initialAccount, activity::defaults::authored());
}

/**
 * Loads build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
bool initialize(void* module,
                const AccountState& initialAccount,
                const activity::defaults::ActivityDefaults& activityDefaults) noexcept {
    g_module = static_cast<HMODULE>(module);
    AccountState runtimeAccount = initialAccount;
    (void)restore_equipment_file(runtimeAccount);
    if (!seed_inventory_runtime_fields(runtimeAccount)
        || !activity::defaults::valid(activityDefaults)) {
        return false;
    }
    if (!build_data::initialize(module, runtime::equipment::configured_hash(runtimeAccount))) {
        return false;
    }
    // A cache hit already has the complete plug relation, so publish canonical profile identities
    // in the first State image.  On a first cache build, snapshot preparation repeats this step
    // after package extraction has published the relation.
    if (build_data::socket_plug_rules_ready()
        && !canonicalize_profile_item_identities(runtimeAccount)) {
        build_data::shutdown();
        return false;
    }
    {
        // The account key is authored, and a truncated one is consistent enough to go unnoticed.
        std::array<char, 96> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=account stage=identity primary=0x%016llX characters=%zu",
                          static_cast<unsigned long long>(runtimeAccount.primarySoid),
                          runtimeAccount.characterCount);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    State initialized{};
    if (!randomize(initialized.signOn.encryptionKey)
        || !randomize(initialized.signOn.authenticationKey)
        || !randomize(initialized.signOn.sessionToken) || !randomize(initialized.bap.nonce)
        || !randomize(initialized.bap.sessionKey) || !randomize(initialized.bap.envelopeIv)) {
        SecureZeroMemory(&initialized, sizeof initialized);
        build_data::shutdown();
        return false;
    }
    initialized.signOn.relayAddress = kLoopbackAddress;
    // The published relay port is the one the listener binds, so both move with one setting.
    initialized.signOn.relayPort = core::settings::get().server.bapPort;
    initialized.signOn.tokenLifetimeSeconds = kDefaultTokenLifetimeSeconds;
    initialized.account = runtimeAccount;
    initialized.activity.defaults = activityDefaults;
    initialized.investment.family5.objectSoid = kGlobalFamily5Soid;
    // Only the override lists come from settings. Identity and gate stay owned by State.
    const Family5State& authored = core::settings::get().initialFamily5;
    initialized.investment.family5.flags = authored.flags;
    initialized.investment.family5.flagCount = authored.flagCount;
    initialized.investment.family5.values = authored.values;
    initialized.investment.family5.valueCount = authored.valueCount;
    // The arm is account-wide and rides the first ws-503, which goes out before any pick. Nothing
    // is selected at boot, so it is armed when any authored character carries the bypass. The
    // per-character objB byte is the other half, and it still decides which character it opens.
    for (std::size_t index = 0; index < runtimeAccount.characterCount; ++index) {
        if (runtimeAccount.characters[index].contentBypass) {
            initialized.investment.family5.contentGateArm = true;
            break;
        }
    }

    // Publish one complete State only after every generated secret is valid.
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state = initialized;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&initialized, sizeof initialized);
    return true;
}

/** Securely erases State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&runtime::storage::g_state, sizeof runtime::storage::g_state);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    build_data::shutdown();
}

/** @return Immutable generated SignOn session fields. */
const SignOnState& sign_on() noexcept {
    return runtime::storage::g_state.signOn;
}

/** Ensures every native profile action source has one unique runtime item-instance key. */
bool ensure_profile_item_identities() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    const bool ready = canonicalize_profile_item_identities(candidate);
    if (ready) {
        runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ready;
}

/**
 * Publishes the bootstrap content-id token read from the installed client.
 * @param token Exactly 16 native bytes.
 * @return True when the complete token is kept for this process.
 */
bool publish_bootstrap_token(std::span<const std::byte> token) noexcept {
    SignOnState& signOn = runtime::storage::g_state.signOn;
    if (token.size() != signOn.bootstrapToken.size()) {
        return false;
    }
    std::copy(token.begin(), token.end(), signOn.bootstrapToken.begin());
    signOn.bootstrapTokenPresent = true;
    return true;
}

/** @return Immutable generated BAP session fields. */
const BapState& bap() noexcept {
    return runtime::storage::g_state.bap;
}

/** @return A copy of the evaluated content state, read under the lock. */
InvestmentState investment_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const InvestmentState snapshot = runtime::storage::g_state.investment;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

} // namespace sunrise::state
