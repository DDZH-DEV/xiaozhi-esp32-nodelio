#pragma once

#include <algorithm>
#include <cstddef>

// SILA's MQTT TLS path needs bounded messages even with PSRAM available.
// Use the same budget for tools/list and the final, wrapped MQTT message.
namespace McpPayloadBudget {
constexpr size_t kMaxWireBytes = 2048;
constexpr size_t kInternalHeadroom = 4096;
constexpr size_t kEnvelopeBytes = 256;  // session, JSON-RPC id and nextCursor

constexpr size_t WireBytes(size_t largest_internal_block) {
    if (largest_internal_block >= 20 * 1024) {
        return 4500;  // Preserve larger MCP results when internal memory is available.
    }
    return largest_internal_block > kInternalHeadroom
               ? std::min(kMaxWireBytes, largest_internal_block - kInternalHeadroom)
               : 0;
}

constexpr size_t ListBytes(size_t largest_internal_block) {
    const size_t wire = WireBytes(largest_internal_block);
    return wire > kEnvelopeBytes ? wire - kEnvelopeBytes : 0;
}
}  // namespace McpPayloadBudget
