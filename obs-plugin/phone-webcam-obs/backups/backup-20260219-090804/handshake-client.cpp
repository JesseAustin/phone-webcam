#define NOMINMAX
#include "handshake-client.h"
#include <obs-module.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include <vector>
#include <string>
#include <sstream>

static constexpr uint16_t HANDSHAKE_PORT = 9001;

std::string HandshakeClient::getBestLocalIp()
{
    // Walk network adapters looking for LAN-reachable addresses:
    // 1. Prefer IPv4 private (192.168.x.x, 10.x.x.x, 172.16–31.x.x) — works reliably on LAN
    // 2. If IPv6, prefer link-local (fe80::) — LAN-local and doesn't require routing through ISP
    // 3. Never use global IPv6 (2xxx:...) for LAN streaming — packets must leave LAN, cross ISP,
    //    and return, causing unreliable delivery and intermittent freezes.
    std::string ipv4Private;
    std::string ipv6LinkLocal;

#ifdef _WIN32
    ULONG bufLen = 15000;
    std::vector<uint8_t> buf(bufLen);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    DWORD res = GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &bufLen);

    if (res == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        res = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufLen);
    }

    if (res != NO_ERROR) {
        blog(LOG_WARNING, "Handshake: GetAdaptersAddresses failed");
        return "";
    }


    for (auto* a = addrs; a != nullptr; a = a->Next) {
        // Skip loopback and non-operational adapters
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;

        for (auto* ua = a->FirstUnicastAddress;
             ua != nullptr; ua = ua->Next) {

            auto* sa = ua->Address.lpSockaddr;

            if (sa->sa_family == AF_INET && ipv4Private.empty()) {
                auto* sin4 = reinterpret_cast<sockaddr_in*>(sa);
                uint32_t ip = ntohl(sin4->sin_addr.s_addr);
                
                // Skip loopback range 127.x.x.x
                if ((ip >> 24) == 127) continue;
                
                // Check for private ranges: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
                bool is_private = 
                    ((ip & 0xFF000000) == 0x0A000000) ||          // 10.0.0.0/8
                    ((ip & 0xFFF00000) == 0xAC100000) ||          // 172.16.0.0/12
                    ((ip & 0xFFFF0000) == 0xC0A80000);            // 192.168.0.0/16

                
                if (!is_private) continue;
                
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sin4->sin_addr,
                          ipStr, sizeof(ipStr));
                ipv4Private = ipStr;
                blog(LOG_INFO, "Handshake: found private IPv4 %s", ipStr);
            }

            if (sa->sa_family == AF_INET6 && ipv6LinkLocal.empty()) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(sa);
                
                // Accept ONLY link-local (fe80::) — NOT global IPv6
                if (!(sin6->sin6_addr.s6_addr[0] == 0xFE &&
                      (sin6->sin6_addr.s6_addr[1] & 0xC0) == 0x80))
                    continue;
                
                char ipStr[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, &sin6->sin6_addr,
                          ipStr, sizeof(ipStr));
                ipv6LinkLocal = ipStr;
                blog(LOG_INFO, "Handshake: found link-local IPv6 %s", ipStr);
            }
        }
    }
#endif

    // Prefer IPv4 private (most reliable on LAN), then IPv6 link-local, reject global IPv6
    if (!ipv4Private.empty()) return ipv4Private;
    if (!ipv6LinkLocal.empty()) return ipv6LinkLocal;
    return "";
}

bool HandshakeClient::sendHandshake(const std::string& phoneIp,
                                    uint16_t streamPort)
{
    // Determine the best local IP (IPv4 private preferred, then IPv6 link-local)
    std::string localIp = getBestLocalIp();
    if (localIp.empty()) {
        blog(LOG_ERROR, "Handshake: could not determine local IP");
        return false;
    }

    blog(LOG_INFO, "Handshake: sending to phone %s — telling it to stream to %s:%d",
         phoneIp.c_str(), localIp.c_str(), streamPort);

    // Build JSON message
    std::ostringstream json;
    json << "{\"ip\":\"" << localIp << "\",\"port\":" << streamPort << "}\n";
    std::string message = json.str();

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        blog(LOG_ERROR, "Handshake: socket creation failed");
        return false;
    }

    // Prepare phone address
    sockaddr_in phoneAddr = {};
    phoneAddr.sin_family = AF_INET;
    phoneAddr.sin_port = htons(HANDSHAKE_PORT);
    if (inet_pton(AF_INET, phoneIp.c_str(), &phoneAddr.sin_addr) != 1) {
        blog(LOG_ERROR, "Handshake: invalid phone IP %s", phoneIp.c_str());
        closesocket(sock);
        return false;
    }

    // Set socket non-blocking for timeout
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    // Start connect
    int res = connect(sock, (sockaddr*)&phoneAddr, sizeof(phoneAddr));
    if (res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        blog(LOG_ERROR, "Handshake: connect failed immediately: %d", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Wait for socket to become writable (connect finished) with 3-second timeout
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv = {3, 0}; // 3 seconds
    res = select(0, nullptr, &wfds, nullptr, &tv);
    if (res <= 0) {
        if (res == 0)
            blog(LOG_ERROR, "Handshake: connect timeout");
        else
            blog(LOG_ERROR, "Handshake: select error: %d", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    // Optional: restore blocking for send
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);

    // Send handshake JSON
    int sent = send(sock, message.c_str(), (int)message.size(), 0);
    closesocket(sock);

    if (sent <= 0) {
        blog(LOG_ERROR, "Handshake: send failed");
        return false;
    }

    blog(LOG_INFO, "Handshake: sent successfully — PC is %s", localIp.c_str());
    return true;
#else
    // Non-Windows not implemented
    return false;
#endif
}