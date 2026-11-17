#ifdef _WIN32
#include "prx/libkernel/Socket/include/SocketConnect.hpp"
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool IsLoopback(const sockaddr* address) {
    if (address->sa_family == AF_INET) return (ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) >> 24) == 127;
    if (address->sa_family == AF_INET6) return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
    return false;
}

u_short Port(const sockaddr* address) {
    return address->sa_family == AF_INET ? reinterpret_cast<const sockaddr_in*>(address)->sin_port : reinterpret_cast<const sockaddr_in6*>(address)->sin6_port;
}

template<typename TTable>
bool ListenerOnPort(ULONG family, u_short port) {
    std::vector<unsigned char> table;
    ULONG size = 0;
    DWORD result = ERROR_INSUFFICIENT_BUFFER;
    while (result == ERROR_INSUFFICIENT_BUFFER) {
        table.resize(size);
        result = GetExtendedTcpTable(table.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (result != NO_ERROR) return true;
    const auto* rows = reinterpret_cast<const TTable*>(table.data());
    for (DWORD i = 0; i < rows->dwNumEntries; ++i) {
        if (static_cast<u_short>(rows->table[i].dwLocalPort) == port) return true;
    }
    return false;
}

bool OffHost(const sockaddr* address) {
    if (address == nullptr || (address->sa_family != AF_INET && address->sa_family != AF_INET6) || IsLoopback(address)) return false;
    if (address->sa_family == AF_INET6) {
        const auto& ip = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&ip) && ip.s6_addr[12] == 127) return false;
    }
    return true;
}

void NoteLocalOnlyOnce(std::atomic<bool>& noted, const char* what) {
    if (!noted.exchange(true, std::memory_order_relaxed)) std::fprintf(stderr, "[net] APS5_NET_LOCAL_ONLY: %s\n", what);
}

}

extern "C" bool NetLocalOnly_nid_no_patch() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_NET_LOCAL_ONLY");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

extern "C" bool KeepBindLocal_nid_no_patch(sockaddr* address) {
    if (!NetLocalOnly_nid_no_patch() || !OffHost(address)) return false;
    if (address->sa_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(address)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        auto& ip = reinterpret_cast<sockaddr_in6*>(address)->sin6_addr;
        const bool mapped = IN6_IS_ADDR_V4MAPPED(&ip);
        ip = in6addr_loopback;
        if (mapped) {
            ip = in6_addr{};
            ip.s6_addr[10] = 0xff;
            ip.s6_addr[11] = 0xff;
            ip.s6_addr[12] = 127;
            ip.s6_addr[15] = 1;
        }
        reinterpret_cast<sockaddr_in6*>(address)->sin6_scope_id = 0;
    }
    static std::atomic<bool> noted{false};
    NoteLocalOnlyOnce(noted, "binds beyond the loopback interface are kept on it");
    return true;
}

extern "C" bool RefuseOffHost_nid_no_patch(const sockaddr* address) {
    if (!NetLocalOnly_nid_no_patch() || !OffHost(address)) return false;
    static std::atomic<bool> noted{false};
    NoteLocalOnlyOnce(noted, "connects and sends beyond the loopback interface fail as unreachable");
    return true;
}

extern "C" bool RefuseHostLookup_nid_no_patch() {
    if (!NetLocalOnly_nid_no_patch()) return false;
    static std::atomic<bool> noted{false};
    NoteLocalOnlyOnce(noted, "host name lookups fail as if no DNS server answered");
    return true;
}

extern "C" bool NoteOffHostSocket_nid_no_patch(const char* call, const sockaddr* address) {
    if (!OffHost(address)) return false;
    static std::atomic<const char*> calls[32]{};
    static std::atomic<unsigned> lines[32]{};
    std::size_t slot = 0;
    for (const char* expected = nullptr; slot < 32; ++slot, expected = nullptr) {
        if (calls[slot].compare_exchange_strong(expected, call) || expected == call) break;
    }
    if (slot == 32 || lines[slot].fetch_add(1, std::memory_order_relaxed) >= 8) return true;
    char text[INET6_ADDRSTRLEN] = "?";
    const void* ip = address->sa_family == AF_INET ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(address)->sin_addr)
                                                   : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
    inet_ntop(address->sa_family, ip, text, sizeof(text));
    std::fprintf(stderr, "[net] %s %s port %u leaves the loopback interface\n", call, text, static_cast<unsigned>(ntohs(Port(address))));
    return true;
}

extern "C" void RefuseClosedLoopbackAtOnce_nid_no_patch(SOCKET socket, const sockaddr* address) {
    if (!IsLoopback(address)) return;
    const u_short port = Port(address);
    if (ListenerOnPort<MIB_TCPTABLE_OWNER_PID>(AF_INET, port) || ListenerOnPort<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, port)) return;
    TCP_INITIAL_RTO_PARAMETERS parameters{TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
    DWORD returned = 0;
    WSAIoctl(socket, SIO_TCP_INITIAL_RTO, &parameters, sizeof(parameters), nullptr, 0, &returned, nullptr, nullptr);
}
#endif
