#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace taskbar_widgets::media_visualizer {

constexpr std::uint32_t kMagic = 0x56575454; // "TTWV"
constexpr std::uint32_t kVersion = 6;
constexpr std::size_t kBandCount = 32;

struct alignas(64) SharedFrame {
    volatile LONG64 sequence{};
    std::uint32_t magic{kMagic};
    std::uint32_t version{kVersion};
    std::uint64_t tickMilliseconds{};
    std::uint32_t sampleRate{};
    std::uint32_t flags{};
    std::array<float, kBandCount> bands{};
};

inline std::wstring MappingName() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return L"Local\\TaskbarWidgets.MediaVisualizer.v6." +
           std::to_wstring(sessionId);
}

inline bool CopyStableFrame(const SharedFrame* source, SharedFrame& destination) {
    if (!source) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        // The Explorer-side view is intentionally FILE_MAP_READ. An
        // InterlockedCompareExchange64 "read" is still a read-modify-write
        // instruction and faults on that mapping. SharedFrame is cache-line
        // aligned and sequence is its first field, so aligned volatile 64-bit
        // loads are atomic on the x64-only builds supported by this project.
        const LONG64 before = source->sequence;
        if ((before & 1) != 0) {
            YieldProcessor();
            continue;
        }

        MemoryBarrier();
        destination.magic = source->magic;
        destination.version = source->version;
        destination.tickMilliseconds = source->tickMilliseconds;
        destination.sampleRate = source->sampleRate;
        destination.flags = source->flags;
        destination.bands = source->bands;
        MemoryBarrier();

        const LONG64 after = source->sequence;
        if (before == after && (after & 1) == 0 &&
            destination.magic == kMagic && destination.version == kVersion) {
            destination.sequence = after;
            return true;
        }
    }

    return false;
}

} // namespace taskbar_widgets::media_visualizer
