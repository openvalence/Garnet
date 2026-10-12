#include <cstring>

#include "esp_random.h"
#include "link/link.h"
#include "lwip/sockets.h"
#include "valence/generated/registry_constants.hpp"
#include "valence/wire/messages/discover.hpp"

namespace garnet {

bool Udp::open() {
    if (_fd >= 0) return true;
    _fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_fd < 0) return false;
    int on = 1;
    setsockopt(_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close();
        return false;
    }
    return true;
}

void Udp::close() {
    if (_fd >= 0) ::close(_fd);
    _fd = -1;
}

bool Udp::sendTo(uint32_t ipv4, std::span<const std::byte> d) {
    if (_fd < 0 || ipv4 == 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(valence::udp_discovery::port));
    a.sin_addr.s_addr = ipv4;
    return sendto(_fd, d.data(), d.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == ssize_t(d.size());
}

bool Udp::sendProbe(uint32_t broadcastIpv4) {
    valence::DiscoverProbe p{};
    p.nonce = esp_random();
    std::array<std::byte, valence::kDiscoverProbeBytes> buf{};
    const size_t n = valence::encodeDiscoverProbe(p, buf);
    return n > 0 && sendTo(broadcastIpv4, std::span<const std::byte>(buf.data(), n));
}

bool Udp::pollReply(uint32_t& ipv4, uint16_t& wsPort, char* name, size_t nameCap, uint64_t& instanceId) {
    if (_fd < 0) return false;
    std::array<std::byte, 128> buf{};
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    for (;;) {
        const ssize_t n = recvfrom(_fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) return false;
        auto r = valence::decodeDiscoverReply(std::span<const std::byte>(buf.data(), size_t(n)));
        if (!r) continue;  // §13.8: not a reply, dropped silently
        ipv4 = from.sin_addr.s_addr;
        wsPort = r.value().ws_port;
        instanceId = r.value().hub_instance_id;
        const std::string_view s = valence::discoverString(r.value().hub_name);
        const size_t k = s.size() < nameCap - 1 ? s.size() : nameCap - 1;
        std::memcpy(name, s.data(), k);
        name[k] = '\0';
        return true;
    }
}

}  // namespace garnet
