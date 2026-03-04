#define NOMINMAX
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "handshake-client.h"
#include "auth.h"
#include <obs-module.h>

#ifdef _WIN32
#include <wincrypt.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#include <vector>
#include <string>
#include <sstream>

static constexpr uint16_t HANDSHAKE_PORT = 9001;

std::string HandshakeClient::getBestLocalIp()
{
    std::string ipv4Private;
    std::string ipv6LinkLocal;

#ifdef _WIN32
    ULONG bufLen = 15000;
    std::vector<uint8_t> buf(bufLen);
    PIP_ADAPTER_ADDRESSES addrs =
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    if (GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufLen) != NO_ERROR) {
        blog(LOG_WARNING, "Handshake: GetAdaptersAddresses failed");
        return "";
    }

    for (auto* a = addrs; a != nullptr; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;

        for (auto* ua = a->FirstUnicastAddress;
             ua != nullptr; ua = ua->Next) {

            auto* sa = ua->Address.lpSockaddr;

            if (sa->sa_family == AF_INET && ipv4Private.empty()) {
                auto* sin4 = reinterpret_cast<sockaddr_in*>(sa);
                uint32_t ip = ntohl(sin4->sin_addr.s_addr);
                if ((ip >> 24) == 127) continue;
                bool is_private =
                    ((ip >> 24) == 10) ||
                    ((ip >> 20) == 0xAC1) ||
                    ((ip >> 16) == 0xC0A8);
                if (!is_private) continue;
                char ipStr[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &sin4->sin_addr, ipStr, sizeof(ipStr));
                ipv4Private = ipStr;
            }

            if (sa->sa_family == AF_INET6 && ipv6LinkLocal.empty()) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(sa);
                if (!(sin6->sin6_addr.s6_addr[0] == 0xFE &&
                      (sin6->sin6_addr.s6_addr[1] & 0xC0) == 0x80))
                    continue;
                char ipStr[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, &sin6->sin6_addr, ipStr, sizeof(ipStr));
                ipv6LinkLocal = ipStr;
            }
        }
    }
#endif

    if (!ipv4Private.empty()) return ipv4Private;
    if (!ipv6LinkLocal.empty()) return ipv6LinkLocal;
    return "";
}

bool HandshakeClient::sendHandshake(const std::string& phoneIp,
                                    uint16_t streamPort,
                                    const std::string& password)
{
    std::string localIp = getBestLocalIp();
    if (localIp.empty()) {
        blog(LOG_ERROR, "Handshake: could not determine local IP");
        return false;
    }

    blog(LOG_INFO, "Handshake: sending to phone %s — streaming to %s:%d (auth=%s)",
         phoneIp.c_str(), localIp.c_str(), streamPort,
         password.empty() ? "none" : "HMAC");

    // -----------------------------------------------------------------------
    // Build JSON:
    //   No password:  {"ip":"…","port":…}
    //   With password: {"ip":"…","port":…,"challenge":"<hex>","hmac":"<hex>"}
    //
    // The challenge is included so the Android side can also verify OBS knows
    // the password (mutual auth). Android computes HMAC of the same challenge
    // and sends it back in its reply — but for this one-shot TCP send we just
    // prove our side.
    // -----------------------------------------------------------------------
    std::ostringstream json;
    json << "{\"ip\":\"" << localIp
         << "\",\"port\":" << streamPort;

    if (!password.empty()) {
        // Generate a fresh challenge
        auto challenge = Auth::generateChallenge();
        std::string challengeHex = Auth::toHex(
            // reuse toHex trick: convert raw bytes to hex via array
            // (challenge is vector, so we copy into array)
            [&]() {
                std::array<uint8_t,32> arr{};
                memcpy(arr.data(), challenge.data(), 32);
                return arr;
            }()
        );

        // Compute HMAC of challenge bytes with the password
        auto mac = Auth::hmac_sha256(password, challenge);
        std::string macHex = Auth::toHex(mac);

        json << ",\"challenge\":\"" << challengeHex
             << "\",\"hmac\":\"" << macHex << "\"";

        blog(LOG_DEBUG, "Handshake: HMAC computed, challenge=%s", challengeHex.c_str());
    }

    json << "}\n";
    std::string message = json.str();

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        blog(LOG_ERROR, "Handshake: socket creation failed");
        return false;
    }

    DWORD timeout = 3000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               (const char*)&timeout, sizeof(timeout));

    sockaddr_in phoneAddr = {};
    phoneAddr.sin_family = AF_INET;
    phoneAddr.sin_port   = htons(HANDSHAKE_PORT);
    inet_pton(AF_INET, phoneIp.c_str(), &phoneAddr.sin_addr);

    if (connect(sock, (sockaddr*)&phoneAddr, sizeof(phoneAddr)) != 0) {
        blog(LOG_ERROR, "Handshake: connect failed: %d", WSAGetLastError());
        closesocket(sock);
        return false;
    }

    int sent = send(sock, message.c_str(), (int)message.size(), 0);
    closesocket(sock);

    if (sent <= 0) {
        blog(LOG_ERROR, "Handshake: send failed");
        return false;
    }

    blog(LOG_INFO, "Handshake: sent successfully");
    return true;
#else
    return false;
#endif
}