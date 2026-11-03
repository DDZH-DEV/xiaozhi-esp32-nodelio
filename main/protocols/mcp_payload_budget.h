#pragma once

#include <algorithm>
#include <cstddef>

// Bound transient MCP allocations on SILA; this is NOT an AES DMA safety check.
// Internal heap size does not imply DMA-capable heap size. The board build uses
// software AES so TLS does not depend on hardware-AES DMA bounce buffers.
// tools/list pagination stays on WireBytes. Wrapped MQTT sends use SendableWire,
// which also lets a short tools/call out when the 4KB headroom zeroes WireBytes.
namespace McpPayloadBudget {
constexpr size_t kMaxWireBytes = 2048;
constexpr size_t kInternalHeadroom = 4096;
constexpr size_t kEnvelopeBytes = 256;  // session, JSON-RPC id and nextCursor
// 云端 TTS 时最大内部块会停在 4KB，与 kInternalHeadroom 相同，WireBytes 变为 0。
// MQTT 缓冲在 PSRAM，TLS 发送缓冲在握手时已经分配。短 tools/call 只要最大块还能
// 盖住报文再加这段暂存，就允许发出。tools/list 分页仍只用 WireBytes。
constexpr size_t kShortReplyMax = 1024;
constexpr size_t kShortReplyScratch = 1536;

constexpr size_t WireBytes(size_t largest_internal_block) {
    if (largest_internal_block >= 20 * 1024) {
        return 4500;  // Preserve larger MCP results when internal memory is available.
    }
    return largest_internal_block > kInternalHeadroom
               ? std::min(kMaxWireBytes, largest_internal_block - kInternalHeadroom)
               : 0;
}

constexpr size_t SendableWire(size_t largest_internal_block) {
    const size_t normal = WireBytes(largest_internal_block);
    if (largest_internal_block <= kShortReplyScratch) {
        return normal;
    }
    const size_t short_cap =
        std::min(kShortReplyMax, largest_internal_block - kShortReplyScratch);
    return normal > short_cap ? normal : short_cap;
}

constexpr size_t ListBytes(size_t largest_internal_block) {
    const size_t wire = WireBytes(largest_internal_block);
    return wire > kEnvelopeBytes ? wire - kEnvelopeBytes : 0;
}
}  // namespace McpPayloadBudget
