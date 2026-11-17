#include "prx/libkernel/Socket/include/SocketConnect.hpp"
#include <cstdio>
#include <cstdlib>
#include <ws2tcpip.h>

namespace {

void Require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", what);
    std::abort();
}

sockaddr_in Inet4(const char* text) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(40000);
    inet_pton(AF_INET, text, &address.sin_addr);
    return address;
}

sockaddr_in6 Inet6(const char* text) {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(40000);
    inet_pton(AF_INET6, text, &address.sin6_addr);
    return address;
}

bool Note(const auto& address) {
    return NoteOffHostSocket_nid_no_patch("bind", reinterpret_cast<const sockaddr*>(&address));
}

}

int main() {
    WSADATA data{};
    Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    Require(!Note(Inet4("127.0.0.1")) && !Note(Inet4("127.4.5.6")), "an IPv4 loopback address was noted");
    Require(!Note(Inet6("::1")) && !Note(Inet6("::ffff:127.0.0.1")), "an IPv6 loopback address was noted");
    Require(Note(Inet4("0.0.0.0")) && Note(Inet4("192.168.0.10")), "an IPv4 address beyond the loopback was not noted");
    Require(Note(Inet6("::")) && Note(Inet6("::ffff:10.0.0.1")), "an IPv6 address beyond the loopback was not noted");
    sockaddr other{};
    other.sa_family = AF_UNIX;
    Require(!NoteOffHostSocket_nid_no_patch("bind", &other) && !NoteOffHostSocket_nid_no_patch("bind", nullptr),
            "a non-IP or missing address was noted");
    std::puts("ok");
    return 0;
}
