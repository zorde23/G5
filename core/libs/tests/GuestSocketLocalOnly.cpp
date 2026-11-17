#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/Api.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int Inet = 2;
constexpr int Stream = 1;
constexpr int Datagram = 2;
constexpr int ErrorNetworkUnreachable = 51;

void Require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "%s (errno %d)\n", what, *__error_nid_postfix());
    std::abort();
}

std::array<std::uint8_t, 16> Address(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    return {16, Inet, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port), a, b, c, d};
}

}

int main() {
    Require(_putenv_s("APS5_NET_LOCAL_ONLY", "1") == 0, "setting APS5_NET_LOCAL_ONLY failed");

    const int datagram = socket_nid_postfix(Inet, Datagram, 0);
    Require(datagram >= 0, "socket failed");
    auto wildcard = Address(0, 0, 0, 0, 0);
    Require(bind_nid_postfix(datagram, wildcard.data(), wildcard.size()) == 0, "a wildcard bind failed");
    std::array<std::uint8_t, 16> bound{};
    std::uint32_t length = bound.size();
    Require(getsockname_nid_postfix(datagram, bound.data(), &length) == 0, "getsockname failed");
    Require(bound[4] == 127 && bound[5] == 0 && bound[6] == 0 && bound[7] == 1, "a wildcard bind did not stay on 127.0.0.1");

    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    const auto offHost = Address(192, 0, 2, 1, 40000);
    *__error_nid_postfix() = 0;
    Require(sendto_nid_postfix(datagram, payload.data(), payload.size(), 0, offHost.data(), offHost.size()) == -1
                && *__error_nid_postfix() == ErrorNetworkUnreachable,
        "a send beyond the loopback interface must fail with ENETUNREACH (51)");
    Require(sendto_nid_postfix(datagram, payload.data(), payload.size(), 0, bound.data(), bound.size()) == 4,
        "a send on the loopback interface must go through");

    const int stream = socket_nid_postfix(Inet, Stream, 0);
    Require(stream >= 0, "socket failed");
    *__error_nid_postfix() = 0;
    Require(connect_nid_postfix(stream, offHost.data(), offHost.size()) == -1 && *__error_nid_postfix() == ErrorNetworkUnreachable,
        "a connect beyond the loopback interface must fail with ENETUNREACH (51)");

    const int unbound = socket_nid_postfix(Inet, Datagram, 0);
    Require(unbound >= 0, "socket failed");
    Require(sendto_nid_postfix(unbound, payload.data(), payload.size(), 0, bound.data(), bound.size()) == 4, "a loopback send from an unbound socket failed");
    std::array<std::uint8_t, 16> implicit{};
    length = implicit.size();
    Require(getsockname_nid_postfix(unbound, implicit.data(), &length) == 0 && implicit[4] == 127,
        "a send from an unbound socket bound it beyond the loopback interface");

    Require(close_nid_postfix(unbound) == 0 && close_nid_postfix(stream) == 0 && close_nid_postfix(datagram) == 0, "close failed");
    std::puts("ok");
    return 0;
}
