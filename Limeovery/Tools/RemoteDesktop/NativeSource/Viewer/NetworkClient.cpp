#include "NetworkClient.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <climits>
#include <iostream>
#include <limits>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    constexpr std::uint32_t H264_MAGIC =
        0x48323634;

    constexpr std::uint32_t MAX_FRAME_SIZE =
        32u * 1024u * 1024u;
}

NetworkClient::NetworkClient()
{
}

NetworkClient::~NetworkClient()
{
    Disconnect();
}

bool NetworkClient::Connect(
    const char* address,
    unsigned short port,
    const std::string& pin)
{
    if (!address || !RemoteDesktopSecurity::IsValidPin(pin))
    {
        return false;
    }

    WSADATA wsaData{};

    const int result =
        WSAStartup(
            MAKEWORD(2, 2),
            &wsaData
        );

    if (result != 0)
    {
        return false;
    }

    winsockInitialized =
        true;

    socketHandle =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );

    if (socketHandle ==
        INVALID_SOCKET)
    {
        Disconnect();
        return false;
    }

    sockaddr_in serverAddress{};

    serverAddress.sin_family =
        AF_INET;

    serverAddress.sin_port =
        htons(port);

    if (InetPtonA(
        AF_INET,
        address,
        &serverAddress.sin_addr
    ) != 1)
    {
        Disconnect();
        return false;
    }

    if (connect(
        socketHandle,
        reinterpret_cast<sockaddr*>(
            &serverAddress
            ),
        sizeof(serverAddress)
    ) == SOCKET_ERROR)
    {
        Disconnect();
        return false;
    }

    BOOL noDelay =
        TRUE;

    setsockopt(
        socketHandle,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(
            &noDelay
            ),
        sizeof(noDelay)
    );

    DWORD handshakeTimeoutMs = 8000;
    setsockopt(
        socketHandle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&handshakeTimeoutMs),
        sizeof(handshakeTimeoutMs));
    setsockopt(
        socketHandle,
        SOL_SOCKET,
        SO_SNDTIMEO,
        reinterpret_cast<const char*>(&handshakeTimeoutMs),
        sizeof(handshakeTimeoutMs));

    RemoteDesktopSecurity::SessionKeys sessionKeys{};
    if (!RemoteDesktopSecurity::ClientHandshake(socketHandle, pin, sessionKeys))
    {
        std::cerr << "PIN authentication failed or Host protocol is incompatible.\n";
        SecureZeroMemory(sessionKeys.serverToClient.data(), sessionKeys.serverToClient.size());
        SecureZeroMemory(sessionKeys.clientToServer.data(), sessionKeys.clientToServer.size());
        Disconnect();
        return false;
    }
    if (!receiveCipher.Initialize(sessionKeys.serverToClient))
    {
        SecureZeroMemory(sessionKeys.serverToClient.data(), sessionKeys.serverToClient.size());
        SecureZeroMemory(sessionKeys.clientToServer.data(), sessionKeys.clientToServer.size());
        Disconnect();
        return false;
    }
    SecureZeroMemory(sessionKeys.serverToClient.data(), sessionKeys.serverToClient.size());
    SecureZeroMemory(sessionKeys.clientToServer.data(), sessionKeys.clientToServer.size());
    receiveSequence = 0;
    DWORD noTimeout = 0;
    setsockopt(
        socketHandle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&noTimeout),
        sizeof(noTimeout));
    setsockopt(
        socketHandle,
        SOL_SOCKET,
        SO_SNDTIMEO,
        reinterpret_cast<const char*>(&noTimeout),
        sizeof(noTimeout));

    std::cout
        << "Connected to Host.\n";

    return true;
}

bool NetworkClient::ReceiveAll(
    unsigned char* data,
    size_t size)
{
    if (socketHandle ==
        INVALID_SOCKET)
    {
        return false;
    }

    size_t totalReceived =
        0;

    while (totalReceived < size)
    {
        const size_t bytesLeft =
            size - totalReceived;

        const int chunk =
            bytesLeft >
            static_cast<size_t>(
                INT_MAX
                )
            ? INT_MAX
            : static_cast<int>(
                bytesLeft
                );

        const int received =
            recv(
                socketHandle,
                reinterpret_cast<char*>(
                    data + totalReceived
                    ),
                chunk,
                0
            );

        if (received <= 0)
        {
            return false;
        }

        totalReceived +=
            static_cast<size_t>(
                received
                );
    }

    return true;
}

bool NetworkClient::ReceiveH264Frame(
    std::vector<unsigned char>& frame,
    unsigned int& frameWidth,
    unsigned int& frameHeight)
{
    frame.clear();

    frameWidth = 0;
    frameHeight = 0;

    H264FrameHeader header{};

    if (!ReceiveAll(
        reinterpret_cast<unsigned char*>(
            &header
            ),
        sizeof(header)
    ))
    {
        return false;
    }

    if (header.magic !=
        H264_MAGIC)
    {
        return false;
    }

    if (header.width == 0 ||
        header.height == 0)
    {
        return false;
    }

    if (header.dataSize <= RemoteDesktopSecurity::GcmTagLength ||
        header.dataSize > MAX_FRAME_SIZE + RemoteDesktopSecurity::GcmTagLength ||
        receiveSequence == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }

    std::vector<unsigned char> encryptedFrame(header.dataSize);

    if (!ReceiveAll(
        encryptedFrame.data(),
        encryptedFrame.size()
    ))
    {
        return false;
    }

    if (!receiveCipher.Decrypt(
        receiveSequence + 1,
        reinterpret_cast<const unsigned char*>(&header),
        sizeof(header),
        encryptedFrame.data(),
        encryptedFrame.size(),
        frame))
    {
        std::cerr << "H.264 frame authentication/decryption failed.\n";
        return false;
    }
    ++receiveSequence;
    frameWidth = header.width;
    frameHeight = header.height;

    return true;
}

void NetworkClient::Disconnect()
{
    if (socketHandle !=
        INVALID_SOCKET)
    {
        shutdown(
            socketHandle,
            SD_BOTH
        );

        closesocket(
            socketHandle
        );

        socketHandle =
            INVALID_SOCKET;
    }

    if (winsockInitialized)
    {
        WSACleanup();

        winsockInitialized =
            false;
    }
}
