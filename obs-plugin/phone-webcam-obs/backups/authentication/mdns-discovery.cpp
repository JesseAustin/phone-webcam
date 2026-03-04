#define NOMINMAX
#include "mdns-discovery.h"
#include <obs-module.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <chrono>
#include <cstring>
#include <vector>
#include <string>
#include <functional>

// mDNS multicast addresses and port
static constexpr const char* MDNS_ADDR_V4  = "224.0.0.251";
static constexpr const char* MDNS_ADDR_V6  = "ff02::fb";
static constexpr uint16_t    MDNS_PORT     = 5353;
static constexpr const char* SERVICE_TYPE  = "_phonewebcam._udp.local";

// -----------------------------------------------------------------------
// Minimal DNS message builder/parser
// -----------------------------------------------------------------------

static void writeDnsName(std::vector<uint8_t>& buf, const std::string& name)
{
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        size_t len = dot - start;
        buf.push_back((uint8_t)len);
        for (size_t i = start; i < dot; i++)
            buf.push_back((uint8_t)name[i]);
        start = dot + 1;
    }
    buf.push_back(0);
}

static std::vector<uint8_t> buildMdnsQuery()
{
    std::vector<uint8_t> pkt;
    pkt.push_back(0x00); pkt.push_back(0x00);
    pkt.push_back(0x00); pkt.push_back(0x00);
    pkt.push_back(0x00); pkt.push_back(0x01);
    pkt.push_back(0x00); pkt.push_back(0x00);
    pkt.push_back(0x00); pkt.push_back(0x00);
    pkt.push_back(0x00); pkt.push_back(0x00);
    writeDnsName(pkt, SERVICE_TYPE);
    pkt.push_back(0x00); pkt.push_back(0x0C);
    pkt.push_back(0x80); pkt.push_back(0x01);
    return pkt;
}

static std::string readDnsName(const uint8_t* pkt, size_t pktLen, size_t& offset)
{
    std::string name;
    int safetyLimit = 64;
    bool jumped = false;
    size_t savedOffset = 0;

    while (offset < pktLen && safetyLimit-- > 0) {
        uint8_t len = pkt[offset];

        if (len == 0) {
            if (!jumped) offset++;
            break;
        }

        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= pktLen) break;
            uint16_t ptr = ((len & 0x3F) << 8) | pkt[offset + 1];
            if (!jumped) { savedOffset = offset + 2; jumped = true; }
            offset = ptr;
            continue;
        }

        offset++;
        if (!name.empty()) name += '.';
        for (uint8_t i = 0; i < len && offset < pktLen; i++)
            name += (char)pkt[offset++];
    }

    if (jumped) offset = savedOffset;
    return name;
}

// -----------------------------------------------------------------------

MdnsDiscovery::MdnsDiscovery()  {}
MdnsDiscovery::~MdnsDiscovery() { stop(); }

void MdnsDiscovery::start(DiscoveryCallback callback)
{
    if (running_) return;
    callback_ = callback;
    running_  = true;
    discovery_thread_ = std::thread(&MdnsDiscovery::discoveryLoop, this);
}

void MdnsDiscovery::stop()
{
    running_ = false;
    if (discovery_thread_.joinable()) {
        blog(LOG_INFO, "mDNS: joining discovery thread on stop");
        discovery_thread_.join();
        discovery_thread_ = std::thread();
    }
}

void MdnsDiscovery::pause()
{
    paused_ = true;
    blog(LOG_INFO, "mDNS discovery paused");
}

void MdnsDiscovery::resume()
{
    last_service_hostname_.clear();
    last_service_port_ = 0;
    paused_ = false;
    blog(LOG_INFO, "mDNS discovery resumed");
}

void MdnsDiscovery::resetDiscoveryState()
{
    last_service_hostname_.clear();
    last_service_port_ = 0;
    blog(LOG_INFO, "mDNS: discovery state reset - next response will trigger callback");
}

void MdnsDiscovery::discoveryLoop()
{
    blog(LOG_INFO, "mDNS discovery started (raw multicast)");

#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        blog(LOG_ERROR, "mDNS: failed to create socket");
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_port        = htons(MDNS_PORT);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) != 0) {
        blog(LOG_ERROR, "mDNS: bind failed: %d", WSAGetLastError());
        closesocket(sock);
        return;
    }

    ip_mreq mreq = {};
    inet_pton(AF_INET, MDNS_ADDR_V4, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char*)&mreq, sizeof(mreq)) != 0) {
        blog(LOG_WARNING, "mDNS: failed to join multicast group: %d", WSAGetLastError());
    }

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in mdnsAddr = {};
    mdnsAddr.sin_family      = AF_INET;
    mdnsAddr.sin_port        = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR_V4, &mdnsAddr.sin_addr);

    auto query     = buildMdnsQuery();
    auto lastQuery = std::chrono::steady_clock::now();

    blog(LOG_INFO, "mDNS: listening on %s:%d", MDNS_ADDR_V4, MDNS_PORT);

    while (running_) {
        // Send query every 1 second (only if not paused)
        auto now = std::chrono::steady_clock::now();
        if (!paused_ && std::chrono::duration_cast<std::chrono::seconds>(
                now - lastQuery).count() >= 1) {
            sendto(sock, (const char*)query.data(), (int)query.size(), 0,
                   (sockaddr*)&mdnsAddr, sizeof(mdnsAddr));
            blog(LOG_INFO, "mDNS: sent PTR query for %s", SERVICE_TYPE);
            lastQuery = now;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval tv = {};
        tv.tv_usec = 100 * 1000; // 100ms

        int selectResult = select(0, &readfds, nullptr, nullptr, &tv);
        if (selectResult <= 0) continue;

        uint8_t buf[4096];
        sockaddr_in sender = {};
        int senderLen = sizeof(sender);

        int received = recvfrom(sock, (char*)buf, sizeof(buf), 0,
                                (sockaddr*)&sender, &senderLen);
        if (received <= 0) continue;

        // If discovery has been paused, ignore incoming responses entirely.
        if (paused_.load()) continue;

        char senderIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sender.sin_addr, senderIp, sizeof(senderIp));

        if (received < 12) continue;

        uint16_t flags = (buf[2] << 8) | buf[3];
        if (!(flags & 0x8000)) continue;

        uint16_t anCount = (buf[6] << 8)  | buf[7];
        uint16_t arCount = (buf[10] << 8) | buf[11];
        uint16_t totalRR = anCount + arCount;

        if (totalRR == 0) continue;

        size_t offset = 12;

        uint16_t qdCount = (buf[4] << 8) | buf[5];
        for (int q = 0; q < qdCount && offset < (size_t)received; q++) {
            readDnsName(buf, received, offset);
            offset += 4;
        }

        uint16_t port = 0;
        std::string targetHost;

        for (int rr = 0; rr < totalRR && offset < (size_t)received; rr++) {
            std::string rrName = readDnsName(buf, received, offset);
            if (offset + 10 > (size_t)received) break;

            uint16_t rrType  = (buf[offset]     << 8) | buf[offset + 1];
            uint16_t rrClass = (buf[offset + 2] << 8) | buf[offset + 3];
            // TTL is offset+4 through offset+7
            uint16_t rdLen   = (buf[offset + 8] << 8) | buf[offset + 9];
            offset += 10;  // type(2) + class(2) + TTL(4) + rdLen(2)

            size_t rdStart = offset;  // remember where rdata begins

            blog(LOG_INFO, "mDNS RR: name=%s type=%d rdLen=%d",
                rrName.c_str(), rrType, rdLen);

            // After parsing PTR record, store the instance name and send SRV query
            if (rrType == 12) { // PTR
                size_t nameOffset = offset;
                std::string target = readDnsName(buf, received, nameOffset);
                blog(LOG_INFO, "mDNS PTR -> %s", target.c_str());
                
                // Send immediate follow-up SRV query for this instance
                if (!target.empty()) {
                    std::vector<uint8_t> srvQuery;
                    srvQuery.push_back(0x00); srvQuery.push_back(0x00); // ID
                    srvQuery.push_back(0x00); srvQuery.push_back(0x00); // flags: standard query
                    srvQuery.push_back(0x00); srvQuery.push_back(0x01); // 1 question
                    srvQuery.push_back(0x00); srvQuery.push_back(0x00); // 0 answers
                    srvQuery.push_back(0x00); srvQuery.push_back(0x00); // 0 authority
                    srvQuery.push_back(0x00); srvQuery.push_back(0x00); // 0 additional
                    writeDnsName(srvQuery, target);
                    srvQuery.push_back(0x00); srvQuery.push_back(0x21); // QTYPE = SRV
                    srvQuery.push_back(0x00); srvQuery.push_back(0x01); // QCLASS = IN
                    
                    sendto(sock, (const char*)srvQuery.data(), (int)srvQuery.size(), 0,
                        (sockaddr*)&mdnsAddr, sizeof(mdnsAddr));
                    blog(LOG_INFO, "mDNS: sent SRV query for %s", target.c_str());
                }
            }
            else if (rrType == 33) { // SRV
                if (offset + 6 <= rdStart + rdLen) {
                    port = (buf[offset + 4] << 8) | buf[offset + 5];
                    size_t nameOffset = offset + 6;
                    targetHost = readDnsName(buf, received, nameOffset);
                    blog(LOG_INFO, "mDNS SRV -> host=%s port=%d", targetHost.c_str(), port);
                }
            }
            else if (rrType == 1) { // A record — grab IP directly
                if (rdLen == 4) {
                    char ipstr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, buf + offset, ipstr, sizeof(ipstr));
                    blog(LOG_INFO, "mDNS A -> %s", ipstr);
                    // If we have a port already, we can call back immediately
                    // with this IP instead of doing a getaddrinfo lookup
                    if (port > 0 && callback_) {
                        last_service_hostname_ = targetHost;
                        last_service_port_ = port;
                        last_service_time_ = std::chrono::steady_clock::now();
                        callback_(ipstr, port);
                    }
                }
            }

            // Always advance by rdLen — never trust name parsing to land correctly
            offset = rdStart + rdLen;
        }

        // If we got a valid streaming port from SRV, check if it's a new service
        if (port > 0) {
            auto now = std::chrono::steady_clock::now();
            constexpr auto DEBOUNCE_MS = 2000;

            bool sameService = (targetHost == last_service_hostname_ && port == last_service_port_);

            if (sameService) {
                // Still suppress rapid duplicates, but allow re-trigger after 10s
                // This lets a re-advertise from Android (after stream drop) re-link OBS
                auto stale = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_service_time_).count();
                if (stale < 6) continue;  // suppress duplicate within 1s window
                // else: fall through and re-fire callback (phone re-advertised)
            }

            // If a previous service was accepted very recently, debounce rapid changes.
            if (!last_service_hostname_.empty()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_service_time_).count();
                if (elapsed < DEBOUNCE_MS) {
                    blog(LOG_INFO, "mDNS: service change ignored due to debounce (%.0fms)", (double)elapsed);
                    continue;
                }
            }

            // Accept new service
            last_service_hostname_ = targetHost;
            last_service_port_     = port;
            last_service_time_     = now;

            blog(LOG_INFO, "mDNS: new service - host=%s port=%d",
                targetHost.c_str(), port);

            // Resolve hostname to IP, then fire callback once
            addrinfo hints = {};
            hints.ai_family   = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* result  = nullptr;

            if (getaddrinfo(targetHost.c_str(), nullptr, &hints, &result) == 0) {
                // Prefer IPv4 addresses when available (more likely routable on LAN).
                addrinfo* chosen = nullptr;
                for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
                    if (p->ai_family == AF_INET) { chosen = p; break; }
                }
                if (!chosen) {
                    // No IPv4 found — accept first IPv6 if present.
                    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
                        if (p->ai_family == AF_INET6) { chosen = p; break; }
                    }
                }

                if (chosen) {
                    char ipstr[INET6_ADDRSTRLEN] = {};
                    void* addrPtr = nullptr;
                    if (chosen->ai_family == AF_INET)
                        addrPtr = &((sockaddr_in*)chosen->ai_addr)->sin_addr;
                    else if (chosen->ai_family == AF_INET6)
                        addrPtr = &((sockaddr_in6*)chosen->ai_addr)->sin6_addr;

                    if (addrPtr) {
                        inet_ntop(chosen->ai_family, addrPtr, ipstr, sizeof(ipstr));
                        blog(LOG_INFO, "mDNS: resolved %s -> %s (family=%d)", targetHost.c_str(), ipstr, chosen->ai_family);
                        if (callback_) callback_(ipstr, port);
                    } else {
                        blog(LOG_WARNING, "mDNS: resolver returned no address for %s", targetHost.c_str());
                        if (callback_) callback_(senderIp, port);
                    }
                } else {
                    // No usable addrinfo entries; fall back to sender IP
                    blog(LOG_WARNING, "mDNS: no usable A/AAAA records for %s, using sender %s", targetHost.c_str(), senderIp);
                    if (callback_) callback_(senderIp, port);
                }

                freeaddrinfo(result);

            } else {
                // DNS lookup failed, fall back to sender IP
                blog(LOG_WARNING, "mDNS: getaddrinfo failed for %s, using sender %s", targetHost.c_str(), senderIp);
                if (callback_) callback_(senderIp, port);
            }
        }
    }

    closesocket(sock);
#endif

    blog(LOG_INFO, "mDNS discovery stopped");
}