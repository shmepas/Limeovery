#include "NetworkServer.h"

#include <windows.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <exception>
#include <iostream>
#include <limits>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    constexpr std::uint32_t H264_MAGIC = 0x48323634;
    constexpr int SocketSendBufferBytes = 64 * 1024;
    constexpr long SelectTimeoutMicroseconds = 20'000;

    bool FindAnnexBStartCode(
        const unsigned char* data,
        std::size_t size,
        std::size_t from,
        std::size_t& nalStart)
    {
        for (std::size_t index = from; index + 3 <= size; ++index)
        {
            if (data[index] != 0 || data[index + 1] != 0)
            {
                continue;
            }

            if (data[index + 2] == 1)
            {
                nalStart = index + 3;
                return true;
            }

            if (index + 4 <= size &&
                data[index + 2] == 0 &&
                data[index + 3] == 1)
            {
                nalStart = index + 4;
                return true;
            }
        }
        return false;
    }

    std::uint32_t ReadBigEndian32(const unsigned char* bytes)
    {
        return (static_cast<std::uint32_t>(bytes[0]) << 24) |
            (static_cast<std::uint32_t>(bytes[1]) << 16) |
            (static_cast<std::uint32_t>(bytes[2]) << 8) |
            static_cast<std::uint32_t>(bytes[3]);
    }
}

NetworkServer::~NetworkServer()
{
    Stop();
}

bool NetworkServer::Start(unsigned short port)
{
    if (listenSocket != INVALID_SOCKET)
    {
        return false;
    }

    WSADATA wsaData{};
    const int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0)
    {
        std::cout << "WSAStartup failed: " << startupResult << "\n";
        return false;
    }
    winsockInitialized = true;

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        std::cout << "socket() failed: " << WSAGetLastError() << "\n";
        Stop();
        return false;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(
        listenSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuseAddress),
        sizeof(reuseAddress));

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
        listenSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)) == SOCKET_ERROR)
    {
        std::cout << "bind() failed: " << WSAGetLastError() << "\n";
        Stop();
        return false;
    }

    if (listen(listenSocket, 1) == SOCKET_ERROR)
    {
        std::cout << "listen() failed: " << WSAGetLastError() << "\n";
        Stop();
        return false;
    }

    std::cout << "Server listening on port " << port << "\n";
    return true;
}

bool NetworkServer::AcceptClient(const std::string& pin)
{
    if (listenSocket == INVALID_SOCKET ||
        clientSocket != INVALID_SOCKET ||
        senderThread.joinable())
    {
        return false;
    }

    if (!RemoteDesktopSecurity::IsValidPin(pin))
    {
        std::cout << "A valid 12-digit device PIN is required.\n";
        return false;
    }

    SOCKET acceptedSocket = INVALID_SOCKET;
    sockaddr_in clientAddress{};
    RemoteDesktopSecurity::SessionKeys sessionKeys{};
    unsigned int failedAuthAttempts = 0;
    while (acceptedSocket == INVALID_SOCKET)
    {
        int clientAddressSize = sizeof(clientAddress);
        SOCKET candidate = accept(
            listenSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &clientAddressSize);

        if (candidate == INVALID_SOCKET)
        {
            std::cout << "accept() failed: " << WSAGetLastError() << "\n";
            return false;
        }

        DWORD handshakeTimeoutMs = 8000;
        setsockopt(
            candidate,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&handshakeTimeoutMs),
            sizeof(handshakeTimeoutMs));
        setsockopt(
            candidate,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&handshakeTimeoutMs),
            sizeof(handshakeTimeoutMs));

        BOOL noDelay = TRUE;
        setsockopt(
            candidate,
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<const char*>(&noDelay),
            sizeof(noDelay));

        if (!RemoteDesktopSecurity::ServerHandshake(candidate, pin, sessionKeys))
        {
            std::cout << "Rejected Viewer: PIN authentication failed. Waiting for a valid connection...\n";
            shutdown(candidate, SD_BOTH);
            closesocket(candidate);
            failedAuthAttempts = std::min(failedAuthAttempts + 1, 16u);
            std::this_thread::sleep_for(std::chrono::milliseconds(
                std::min(failedAuthAttempts, 8u) * 500u));
            continue;
        }

        acceptedSocket = candidate;
    }

    DWORD noTimeout = 0;
    setsockopt(
        acceptedSocket,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&noTimeout),
        sizeof(noTimeout));
    setsockopt(
        acceptedSocket,
        SOL_SOCKET,
        SO_SNDTIMEO,
        reinterpret_cast<const char*>(&noTimeout),
        sizeof(noTimeout));

    BOOL noDelay = TRUE;
    if (setsockopt(
        acceptedSocket,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&noDelay),
        sizeof(noDelay)) == SOCKET_ERROR)
    {
        std::cout << "Could not enable TCP_NODELAY: " << WSAGetLastError() << "\n";
    }

    int sendBufferBytes = SocketSendBufferBytes;
    if (setsockopt(
        acceptedSocket,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&sendBufferBytes),
        sizeof(sendBufferBytes)) == SOCKET_ERROR)
    {
        std::cout << "Could not set SO_SNDBUF: " << WSAGetLastError() << "\n";
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(acceptedSocket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        std::cout << "ioctlsocket(FIONBIO) failed: " << WSAGetLastError() << "\n";
        closesocket(acceptedSocket);
        return false;
    }

    if (!senderCipher.Initialize(sessionKeys.serverToClient))
    {
        std::cout << "Could not initialize AES-GCM stream encryption.\n";
        closesocket(acceptedSocket);
        return false;
    }
    sendSequence = 0;
    SecureZeroMemory(sessionKeys.serverToClient.data(), sessionKeys.serverToClient.size());
    SecureZeroMemory(sessionKeys.clientToServer.data(), sessionKeys.clientToServer.size());

    clientSocket = acceptedSocket;
    clientConnected.store(true, std::memory_order_release);
    senderStop.store(false, std::memory_order_release);
    keyFrameRequestPending.store(true, std::memory_order_release);
    keyFrameDelivered.store(false, std::memory_order_release);
    keyFrameInFlight.store(false, std::memory_order_release);
    senderBusy.store(false, std::memory_order_release);
    decoderRecoveryNeeded.store(true, std::memory_order_release);
    awaitingKeyFrame = true;
    keyFramePending = false;
    droppedWhileKeyFramePending = false;
    frameRing.Clear();

    char addressBuffer[INET_ADDRSTRLEN]{};
    if (InetNtopA(
        AF_INET,
        &clientAddress.sin_addr,
        addressBuffer,
        sizeof(addressBuffer)))
    {
        std::cout << "Client connected from " << addressBuffer << ":"
            << ntohs(clientAddress.sin_port) << "\n";
    }
    else
    {
        std::cout << "Client connected.\n";
    }

    try
    {
        senderThread = std::thread(&NetworkServer::SenderThreadMain, this);
    }
    catch (const std::exception& error)
    {
        std::cout << "Could not start network sender thread: " << error.what() << "\n";
        clientConnected.store(false, std::memory_order_release);
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

bool NetworkServer::SendH264Frame(
    std::vector<unsigned char>&& encodedFrame,
    unsigned int frameWidth,
    unsigned int frameHeight)
{
    if (encodedFrame.empty() ||
        encodedFrame.size() > MaxFrameSize ||
        frameWidth == 0 ||
        frameHeight == 0 ||
        !clientConnected.load(std::memory_order_acquire))
    {
        return false;
    }

    ProcessKeyFrameDeliveryEvent();

    const bool isKeyFrame = ContainsIdrNal(encodedFrame.data(), encodedFrame.size());
    H264EncodedPacket packet{};
    packet.header.magic = H264_MAGIC;
    packet.header.width = frameWidth;
    packet.header.height = frameHeight;
    packet.header.dataSize = static_cast<std::uint32_t>(encodedFrame.size());
    packet.isKeyFrame = isKeyFrame;

    if (awaitingKeyFrame)
    {
        if (!isKeyFrame)
        {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            if (keyFramePending)
            {
                droppedWhileKeyFramePending = true;
            }
            return true;
        }

        if (keyFramePending)
        {
            // An IDR is already waiting in the ring or in the network worker.
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        keyFramePending = true;
        droppedWhileKeyFramePending = false;
        keyFrameInFlight.store(true, std::memory_order_release);
        keyFrameRequestPending.store(false, std::memory_order_release);
    }
    else if (frameRing.ApproximateSize() >= H264FrameRingBuffer::Capacity)
    {
        // Pre-arm recovery before enqueue: the consumer must not start sending
        // a P-frame from a reference chain that is about to be overwritten.
        awaitingKeyFrame = true;
        decoderRecoveryNeeded.store(true, std::memory_order_release);
        droppedWhileKeyFramePending = false;
        if (isKeyFrame)
        {
            keyFramePending = true;
            keyFrameInFlight.store(true, std::memory_order_release);
            keyFrameRequestPending.store(false, std::memory_order_release);
        }
        else
        {
            keyFramePending = false;
            keyFrameRequestPending.store(true, std::memory_order_release);
        }
    }

    decoderRecoveryNeeded.store(awaitingKeyFrame, std::memory_order_release);
    packet.payload = std::move(encodedFrame);

    bool droppedOldest = false;
    if (!frameRing.TryPush(std::move(packet), droppedOldest))
    {
        droppedFrames.fetch_add(1, std::memory_order_relaxed);
        awaitingKeyFrame = true;
        keyFramePending = false;
        keyFrameInFlight.store(false, std::memory_order_release);
        droppedWhileKeyFramePending = false;
        decoderRecoveryNeeded.store(true, std::memory_order_release);
        keyFrameRequestPending.store(true, std::memory_order_release);
        return true;
    }

    if (droppedOldest)
    {
        droppedFrames.fetch_add(1, std::memory_order_relaxed);
        if (!awaitingKeyFrame)
        {
            // Defensive path for a concurrent consumer size change. Normally
            // the full-ring precheck above enters recovery before TryPush.
            awaitingKeyFrame = true;
            decoderRecoveryNeeded.store(true, std::memory_order_release);
            keyFramePending = false;
            keyFrameRequestPending.store(true, std::memory_order_release);
        }
    }

    return true;
}

bool NetworkServer::ConsumeKeyFrameRequest() noexcept
{
    return keyFrameRequestPending.exchange(false, std::memory_order_acq_rel);
}

NetworkServerStats NetworkServer::GetStats() const
{
    NetworkServerStats stats{};
    stats.transmittedFrames = transmittedFrames.load(std::memory_order_relaxed);
    stats.droppedFrames = droppedFrames.load(std::memory_order_relaxed);
    stats.bytesSent = bytesSent.load(std::memory_order_relaxed);
    stats.maxSendDurationMs = maxSendDurationMs.load(std::memory_order_relaxed);
    stats.outstandingFrames = frameRing.ApproximateSize() +
        (senderBusy.load(std::memory_order_acquire) ? 1u : 0u);
    stats.awaitingKeyFrame = decoderRecoveryNeeded.load(std::memory_order_acquire);
    stats.keyFrameInFlight = keyFrameInFlight.load(std::memory_order_acquire);
    stats.clientConnected = clientConnected.load(std::memory_order_acquire);
    return stats;
}

void NetworkServer::ProcessKeyFrameDeliveryEvent()
{
    if (!keyFrameDelivered.exchange(false, std::memory_order_acq_rel) ||
        !keyFramePending)
    {
        return;
    }

    keyFramePending = false;
    if (droppedWhileKeyFramePending)
    {
        // P-frames generated while the IDR was stuck in the queue were omitted.
        // Ask for another IDR after the first one reaches the socket.
        droppedWhileKeyFramePending = false;
        awaitingKeyFrame = true;
        decoderRecoveryNeeded.store(true, std::memory_order_release);
        keyFrameRequestPending.store(true, std::memory_order_release);
    }
    else
    {
        awaitingKeyFrame = false;
        decoderRecoveryNeeded.store(false, std::memory_order_release);
    }
}

void NetworkServer::SenderThreadMain()
{
    H264EncodedPacket packet{};
    while (frameRing.WaitPop(packet, senderStop))
    {
        senderBusy.store(true, std::memory_order_release);

        if (decoderRecoveryNeeded.load(std::memory_order_acquire) && !packet.isKeyFrame)
        {
            senderBusy.store(false, std::memory_order_release);
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            packet = H264EncodedPacket{};
            continue;
        }

        const auto sendStart = std::chrono::steady_clock::now();
        const bool sent = SendPacket(packet);
        const auto sendEnd = std::chrono::steady_clock::now();
        senderBusy.store(false, std::memory_order_release);

        if (!sent)
        {
            MarkDisconnected();
            return;
        }

        const auto sendDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            sendEnd - sendStart).count();
        UpdateMaximum(
            maxSendDurationMs,
            static_cast<std::uint64_t>(std::max<std::int64_t>(sendDuration, 0)));
        transmittedFrames.fetch_add(1, std::memory_order_relaxed);
        if (packet.isKeyFrame)
        {
            if (keyFrameInFlight.exchange(false, std::memory_order_acq_rel))
            {
                decoderRecoveryNeeded.store(false, std::memory_order_release);
            }
            keyFrameDelivered.store(true, std::memory_order_release);
        }
        packet = H264EncodedPacket{};
    }
}

bool NetworkServer::SendPacket(const H264EncodedPacket& packet)
{
    if (sendSequence == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }

    H264FrameHeader encryptedHeader = packet.header;
    if (packet.payload.size() > MaxFrameSize - RemoteDesktopSecurity::GcmTagLength)
    {
        return false;
    }
    encryptedHeader.dataSize = static_cast<std::uint32_t>(
        packet.payload.size() + RemoteDesktopSecurity::GcmTagLength);

    std::vector<unsigned char> encryptedPayload;
    if (!senderCipher.Encrypt(
        sendSequence + 1,
        reinterpret_cast<const unsigned char*>(&encryptedHeader),
        sizeof(encryptedHeader),
        packet.payload.data(),
        packet.payload.size(),
        encryptedPayload))
    {
        std::cout << "AES-GCM encryption failed for an H.264 frame.\n";
        return false;
    }
    ++sendSequence;

    return SendBytes(
        reinterpret_cast<const unsigned char*>(&encryptedHeader),
        sizeof(encryptedHeader)) &&
        SendBytes(encryptedPayload.data(), encryptedPayload.size());
}

bool NetworkServer::SendBytes(const unsigned char* data, std::size_t size)
{
    if (size == 0)
    {
        return true;
    }

    std::size_t sentTotal = 0;
    while (sentTotal < size)
    {
        if (senderStop.load(std::memory_order_acquire) ||
            !clientConnected.load(std::memory_order_acquire))
        {
            return false;
        }

        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(clientSocket, &writeSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = SelectTimeoutMicroseconds;

        const int ready = select(0, nullptr, &writeSet, nullptr, &timeout);
        if (ready == 0)
        {
            continue;
        }
        if (ready == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAEINTR)
            {
                continue;
            }
            std::cout << "select() for socket send failed: " << error << "\n";
            return false;
        }

        const std::size_t remaining = size - sentTotal;
        const int chunk = static_cast<int>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(INT_MAX)));
        const int sent = send(
            clientSocket,
            reinterpret_cast<const char*>(data + sentTotal),
            chunk,
            0);

        if (sent == SOCKET_ERROR)
        {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR)
            {
                continue;
            }
            std::cout << "send() failed: " << error << "\n";
            return false;
        }
        if (sent == 0)
        {
            std::cout << "send() returned 0; the Viewer closed the connection.\n";
            return false;
        }
        sentTotal += static_cast<std::size_t>(sent);
        bytesSent.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
    }
    return true;
}

void NetworkServer::MarkDisconnected()
{
    clientConnected.store(false, std::memory_order_release);
    senderStop.store(true, std::memory_order_release);
    frameRing.NotifyAll();
}

bool NetworkServer::ContainsIdrNal(const unsigned char* data, std::size_t size)
{
    if (!data || size == 0)
    {
        return false;
    }

    bool hasAnnexBStartCode = false;
    std::size_t searchFrom = 0;
    std::size_t nalStart = 0;
    while (FindAnnexBStartCode(data, size, searchFrom, nalStart))
    {
        hasAnnexBStartCode = true;
        if (nalStart < size && (data[nalStart] & 0x1f) == 5)
        {
            return true;
        }
        searchFrom = nalStart;
    }
    if (hasAnnexBStartCode)
    {
        return false;
    }

    // Some MFTs expose AVC length-prefixed NAL units instead of Annex B.
    std::size_t offset = 0;
    while (offset + sizeof(std::uint32_t) <= size)
    {
        const std::uint32_t nalLength = ReadBigEndian32(data + offset);
        offset += sizeof(std::uint32_t);
        if (nalLength == 0 || nalLength > size - offset)
        {
            break;
        }
        if ((data[offset] & 0x1f) == 5)
        {
            return true;
        }
        offset += nalLength;
    }

    return false;
}

void NetworkServer::UpdateMaximum(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value)
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
        !target.compare_exchange_weak(
            current,
            value,
            std::memory_order_relaxed,
            std::memory_order_relaxed))
    {
    }
}

void NetworkServer::Stop()
{
    senderStop.store(true, std::memory_order_release);
    frameRing.NotifyAll();

    if (clientSocket != INVALID_SOCKET)
    {
        shutdown(clientSocket, SD_BOTH);
    }

    if (senderThread.joinable())
    {
        senderThread.join();
    }

    const std::size_t queuedFrames = frameRing.ApproximateSize();
    if (queuedFrames != 0)
    {
        droppedFrames.fetch_add(queuedFrames, std::memory_order_relaxed);
        frameRing.Clear();
    }

    clientConnected.store(false, std::memory_order_release);
    if (clientSocket != INVALID_SOCKET)
    {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }

    if (listenSocket != INVALID_SOCKET)
    {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (winsockInitialized)
    {
        WSACleanup();
        winsockInitialized = false;
    }
}
