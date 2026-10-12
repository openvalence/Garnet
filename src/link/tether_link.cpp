#include <cstdio>
#include <cstring>

#include "link/link.h"
#include "mbedtls/base64.h"

namespace garnet {

TetherLink::TetherLink() : _rx(48) {}

bool TetherLink::write(std::span<const std::byte> frame) {
    std::array<unsigned char, (valence::kFrameBufferCapacity + 2) / 3 * 4 + 1> b64{};
    size_t n = 0;
    if (mbedtls_base64_encode(b64.data(), b64.size(), &n, reinterpret_cast<const unsigned char*>(frame.data()),
                              frame.size()) != 0) {
        return false;
    }
    // One printf call: the stdout lock keeps the line whole between log lines.
    std::printf("@V %.*s\n", int(n), reinterpret_cast<const char*>(b64.data()));
    return true;
}

void TetherLink::pushLine(const char* b64) {
    std::array<unsigned char, valence::kFrameBufferCapacity> raw{};
    size_t n = 0;
    if (mbedtls_base64_decode(raw.data(), raw.size(), &n, reinterpret_cast<const unsigned char*>(b64),
                              std::strlen(b64)) != 0) {
        return;
    }
    _rx.push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()), n));
}

valence::TransportProperties TetherLink::properties() const {
    valence::TransportProperties p{};
    p.mtu = uint16_t(valence::kFrameBufferCapacity);
    p.ordered = true;
    p.reliable = true;
    p.congestion = valence::CongestionSignal::QueueWatermark;
    return p;
}

}  // namespace garnet
