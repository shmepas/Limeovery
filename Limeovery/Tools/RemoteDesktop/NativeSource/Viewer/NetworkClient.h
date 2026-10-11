#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include <cstdint>
#include <vector>

#include "../Common/RemoteDesktopSecurity.h"

#pragma pack(push, 1)

struct H264FrameHeader
{
    std::uint32_t magic;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t dataSize;
};

#pragma pack(pop)

class NetworkClient
{
public:
    NetworkClient();
    ~NetworkClient();

    bool Connect(
        const char* address,
        unsigned short port,
        const std::string& pin
    );

    bool ReceiveH264Frame(
        std::vector<unsigned char>& frame,
        unsigned int& width,
        unsigned int& height
    );

    void Disconnect();

private:
    bool ReceiveAll(
        unsigned char* data,
        size_t size
    );

private:
    SOCKET socketHandle =
        INVALID_SOCKET;

    bool winsockInitialized =
        false;

    RemoteDesktopSecurity::AesGcmCipher receiveCipher;
    std::uint64_t receiveSequence = 0;
};
