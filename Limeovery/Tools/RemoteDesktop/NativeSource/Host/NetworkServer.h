#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "H264FrameRingBuffer.h"
#include "../Common/RemoteDesktopSecurity.h"

struct NetworkServerStats
{
    std::uint64_t transmittedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t maxSendDurationMs = 0;
    std::size_t outstandingFrames = 0;
    bool awaitingKeyFrame = true;
    bool keyFrameInFlight = false;
    bool clientConnected = false;
};

class NetworkServer
{
public:
    NetworkServer() = default;
    ~NetworkServer();

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    bool Start(unsigned short port);
    bool AcceptClient(const std::string& pin);

    // Transfers ownership of a compressed access unit to the bounded ring.
    // This call never waits for socket writability and is safe on the encoder
    // thread; a brief ring-lock collision drops this new packet immediately.
    // A frame dropped during congestion still returns true while the client is
    // connected; false means the connection is unavailable or input invalid.
    bool SendH264Frame(
        std::vector<unsigned char>&& encodedFrame,
        unsigned int frameWidth,
        unsigned int frameHeight);

    // Host polls this on its encoder thread and performs the MFT call there.
    bool ConsumeKeyFrameRequest() noexcept;
    NetworkServerStats GetStats() const;
    void Stop();

private:
    void SenderThreadMain();
    bool SendPacket(const H264EncodedPacket& packet);
    bool SendBytes(const unsigned char* data, std::size_t size);
    void MarkDisconnected();
    static bool ContainsIdrNal(const unsigned char* data, std::size_t size);
    static void UpdateMaximum(std::atomic<std::uint64_t>& target, std::uint64_t value);
    void ProcessKeyFrameDeliveryEvent();

    static constexpr std::uint32_t MaxFrameSize = 32u * 1024u * 1024u;

    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    bool winsockInitialized = false;

    std::atomic<bool> senderStop{ false };
    std::atomic<bool> clientConnected{ false };
    std::atomic<bool> keyFrameRequestPending{ false };
    std::atomic<bool> senderBusy{ false };
    std::atomic<bool> decoderRecoveryNeeded{ true };
    std::atomic<bool> keyFrameDelivered{ false };
    std::atomic<bool> keyFrameInFlight{ false };
    H264FrameRingBuffer frameRing;
    RemoteDesktopSecurity::AesGcmCipher senderCipher;
    std::uint64_t sendSequence = 0;

    // These flags are producer-thread-owned. The network worker only signals
    // keyFrameDelivered after a complete IDR packet reaches the socket.
    bool awaitingKeyFrame = true;
    bool keyFramePending = false;
    bool droppedWhileKeyFramePending = false;
    std::thread senderThread;

    std::atomic<std::uint64_t> transmittedFrames{ 0 };
    std::atomic<std::uint64_t> droppedFrames{ 0 };
    std::atomic<std::uint64_t> bytesSent{ 0 };
    std::atomic<std::uint64_t> maxSendDurationMs{ 0 };
};
