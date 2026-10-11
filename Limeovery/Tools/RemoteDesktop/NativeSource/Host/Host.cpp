#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mfapi.h>
#include <winrt/base.h>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "ScreenCapture.h"
#include "H264Encoder.h"
#include "NetworkServer.h"
#include "../Common/RemoteDesktopSecurity.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")

int main(int argc, char* argv[])
{
    // Host is launched with redirected stdout/stderr by Limeovery. Flush every
    // diagnostic immediately so startup failures are available in Host.log.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::wcout << std::unitbuf;

    // Keep the host's diagnostic console hidden when launched directly or by an
    // older Limeovery build. The Limeovery window reports the Host process state.
    if (const HWND consoleWindow = GetConsoleWindow())
    {
        ShowWindow(consoleWindow, SW_HIDE);
    }

    unsigned short listenPort = 5010;
    bool pinFromStandardInput = false;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--port") == 0)
        {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || parsed == 0 || parsed > 65535)
            {
                std::cerr << "Invalid --port value. Expected 1..65535.\n";
                return 2;
            }
            listenPort = static_cast<unsigned short>(parsed);
        }
    }

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--pin-stdin") == 0)
        {
            pinFromStandardInput = true;
        }
    }

    std::string devicePin;
    if (!pinFromStandardInput || !std::getline(std::cin, devicePin) ||
        !RemoteDesktopSecurity::IsValidPin(devicePin))
    {
        std::cerr << "Host requires a 12-digit PIN through its redirected standard input.\n";
        return 2;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    std::cout << "=============================\n"
        << "       Remote Desktop Host\n"
        << "=============================\n\n";

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        std::cout << "MFStartup failed: 0x" << std::hex
            << static_cast<unsigned long>(hr) << std::dec << "\n";
        return 1;
    }

    ScreenCapture capture;
    if (!capture.Initialize() || !capture.Start())
    {
        capture.Stop();
        MFShutdown();
        return 1;
    }

    std::cout << "Screen capture started.\n";

    Microsoft::WRL::ComPtr<ID3D11Texture2D> bgraTexture;
    UINT width = 0;
    UINT height = 0;
    LONGLONG captureTime100ns = 0;
    while (!capture.GetLatestFrame(
        bgraTexture,
        width,
        height,
        captureTime100ns))
    {
        capture.WaitForNewFrame(capture.GetFrameCount(), 100);
    }

    std::cout << "Capture resolution: " << width << "x" << height << "\n";

    constexpr UINT maxTargetFps = 120;
    constexpr UINT minTargetFps = 15;
    constexpr UINT maxTargetBitrate = 4'000'000;
    constexpr UINT minTargetBitrate = 500'000;
    // Start with a sustainable desktop-streaming profile and ramp upward only
    // after several clean network intervals. This avoids an initial burst of
    // 120-FPS traffic filling TCP before the feedback controller can react.
    UINT adaptiveTargetFps = 60;
    UINT adaptiveTargetBitrate = 2'000'000;

    H264Encoder encoder;
    if (!encoder.Initialize(
        capture.GetDevice(),
        width,
        height,
        maxTargetFps,
        adaptiveTargetBitrate))
    {
        capture.Stop();
        MFShutdown();
        return 1;
    }

    NetworkServer server;
    if (!server.Start(listenPort))
    {
        encoder.Shutdown();
        capture.Stop();
        MFShutdown();
        return 1;
    }

    // The UI waits for this line instead of treating Process.Start as proof
    // that capture, encoder and network listener all initialized successfully.
    std::cout << "LIMEOVERY_HOST_READY\n";

    std::cout << "Waiting for Viewer on port " << listenPort << "...\n";
    if (!server.AcceptClient(devicePin))
    {
        SecureZeroMemory(devicePin.data(), devicePin.size());
        server.Stop();
        encoder.Shutdown();
        capture.Stop();
        MFShutdown();
        return 1;
    }
    SecureZeroMemory(devicePin.data(), devicePin.size());

    std::cout << "Viewer connected.\n";

    // AcceptClient asks for an IDR so the Viewer can initialize its decoder
    // without waiting for the next periodic keyframe.
    if (server.ConsumeKeyFrameRequest())
    {
        encoder.RequestKeyFrame();
    }

    // Do not pace at 30 FPS. GraphicsCapture reports frames when the source
    // display updates; each iteration consumes the newest frame to keep latency
    // bounded if encoding or the network falls behind.
    unsigned long long lastCaptureFrame = capture.GetFrameCount();
    unsigned long long submittedFrames = 0;
    unsigned long long capturedFrames = 0;
    unsigned long long skippedCaptureFrames = 0;
    unsigned long long networkWaitSkippedFrames = 0;
    unsigned long long fpsLimitSkippedFrames = 0;
    unsigned long long encodeAttempts = 0;
    double encodeTimeMs = 0.0;
    double enqueueTimeMs = 0.0;
    double encodedBytes = 0.0;
    double maxEncodeTimeMs = 0.0;
    double maxEnqueueTimeMs = 0.0;
    double conversionTimeMs = 0.0;
    double waitForInputTimeMs = 0.0;
    double sampleSetupTimeMs = 0.0;
    double processInputTimeMs = 0.0;
    double waitForOutputTimeMs = 0.0;
    double processOutputTimeMs = 0.0;
    unsigned int stableNetworkIntervals = 0;
    auto statisticsTime = std::chrono::steady_clock::now();
    auto nextEncodeTime = statisticsTime;
    size_t lastH264Size = 0;
    std::uint64_t lastReportedTransmittedFrames = 0;
    std::uint64_t lastReportedDroppedFrames = 0;
    std::uint64_t lastReportedBytesSent = 0;

    auto reportStatistics = [&]()
    {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - statisticsTime).count();
        if (elapsed < 1.0)
        {
            return;
        }

        const double encodeDivisor = static_cast<double>(std::max<unsigned long long>(encodeAttempts, 1));
        const double enqueueDivisor = static_cast<double>(std::max<unsigned long long>(submittedFrames, 1));
        const NetworkServerStats networkStats = server.GetStats();
        const double networkFramesPerSecond = static_cast<double>(
            networkStats.transmittedFrames - lastReportedTransmittedFrames) / elapsed;
        const std::uint64_t droppedThisInterval =
            networkStats.droppedFrames - lastReportedDroppedFrames;
        const std::uint64_t bytesSentThisInterval =
            networkStats.bytesSent - lastReportedBytesSent;
        const double socketBitsPerSecond =
            static_cast<double>(bytesSentThisInterval) * 8.0 / elapsed;

        const bool networkStalled = networkStats.outstandingFrames > 0 &&
            bytesSentThisInterval == 0;
        const bool congested = droppedThisInterval > 0 ||
            networkStats.outstandingFrames >= 3 ||
            networkStalled;
        if (congested)
        {
            stableNetworkIntervals = 0;
            adaptiveTargetFps = std::max(
                minTargetFps,
                static_cast<UINT>((static_cast<std::uint64_t>(adaptiveTargetFps) * 3) / 4));

            const double linkLimitedBitrate = socketBitsPerSecond > 0.0
                ? socketBitsPerSecond * 0.70
                : static_cast<double>(adaptiveTargetBitrate) * 0.75;
            const UINT bitrateStepDown = static_cast<UINT>(
                static_cast<std::uint64_t>(adaptiveTargetBitrate) * 3 / 4);
            const UINT desiredBitrate = std::max(
                minTargetBitrate,
                std::min(bitrateStepDown, static_cast<UINT>(std::max<double>(
                    0.0,
                    std::min<double>(linkLimitedBitrate, maxTargetBitrate)))));
            if (desiredBitrate < adaptiveTargetBitrate &&
                encoder.SetBitrate(desiredBitrate))
            {
                adaptiveTargetBitrate = desiredBitrate;
            }
        }
        else if (!networkStats.awaitingKeyFrame &&
            !networkStats.keyFrameInFlight &&
            networkStats.outstandingFrames <= 1 &&
            (networkStats.outstandingFrames == 0 || bytesSentThisInterval > 0) &&
            (networkFramesPerSecond > 0.0 || bytesSentThisInterval > 0))
        {
            ++stableNetworkIntervals;
            if (stableNetworkIntervals >= 5)
            {
                stableNetworkIntervals = 0;
                adaptiveTargetFps = std::min(maxTargetFps, adaptiveTargetFps + 5);
                const UINT desiredBitrate = std::min(
                    maxTargetBitrate,
                    adaptiveTargetBitrate + 200'000);
                if (desiredBitrate > adaptiveTargetBitrate &&
                    encoder.SetBitrate(desiredBitrate))
                {
                    adaptiveTargetBitrate = desiredBitrate;
                }
            }
        }
        else
        {
            stableNetworkIntervals = 0;
        }

        std::cout << "Encoded output: " << (submittedFrames / elapsed)
            << " frames/s, capture: " << (capturedFrames / elapsed)
            << " frames/s, skipped capture frames: " << skippedCaptureFrames
            << ", network-pause/fps-limit skips: " << networkWaitSkippedFrames
            << "/" << fpsLimitSkippedFrames
            << ", encode avg/max: " << (encodeTimeMs / encodeDivisor) << "/"
            << maxEncodeTimeMs << " ms, phases convert/input-wait/sample/ProcessInput/output-wait/ProcessOutput: "
            << conversionTimeMs / encodeDivisor << "/"
            << waitForInputTimeMs / encodeDivisor << "/"
            << sampleSetupTimeMs / encodeDivisor << "/"
            << processInputTimeMs / encodeDivisor << "/"
            << waitForOutputTimeMs / encodeDivisor << "/"
            << processOutputTimeMs / encodeDivisor << " ms, queue submit avg/max: "
            << (enqueueTimeMs / enqueueDivisor) << "/" << maxEnqueueTimeMs
            << " ms, socket: " << networkFramesPerSecond << " frames/s, throughput: "
            << (socketBitsPerSecond / 1'000'000.0) << " Mbps, pending: "
            << networkStats.outstandingFrames << "/3, dropped: "
            << networkStats.droppedFrames << " (+" << droppedThisInterval
            << "), max packet send: " << networkStats.maxSendDurationMs
            << " ms, encoded payload: " << ((encodedBytes * 8.0 / elapsed) / 1'000'000.0)
            << " Mbps, awaiting IDR: " << (networkStats.awaitingKeyFrame ? "yes" : "no")
            << ", IDR in flight: " << (networkStats.keyFrameInFlight ? "yes" : "no")
            << ", encoder target: " << adaptiveTargetFps << " FPS / "
            << (adaptiveTargetBitrate / 1'000'000.0) << " Mbps"
            << ", last H.264 size: " << lastH264Size << " bytes\n";

        submittedFrames = 0;
        capturedFrames = 0;
        skippedCaptureFrames = 0;
        networkWaitSkippedFrames = 0;
        fpsLimitSkippedFrames = 0;
        encodeAttempts = 0;
        encodeTimeMs = 0.0;
        enqueueTimeMs = 0.0;
        encodedBytes = 0.0;
        maxEncodeTimeMs = 0.0;
        maxEnqueueTimeMs = 0.0;
        conversionTimeMs = 0.0;
        waitForInputTimeMs = 0.0;
        sampleSetupTimeMs = 0.0;
        processInputTimeMs = 0.0;
        waitForOutputTimeMs = 0.0;
        processOutputTimeMs = 0.0;
        lastH264Size = 0;
        lastReportedTransmittedFrames = networkStats.transmittedFrames;
        lastReportedDroppedFrames = networkStats.droppedFrames;
        lastReportedBytesSent = networkStats.bytesSent;
        statisticsTime = now;
    };

    while (true)
    {
        if (!capture.WaitForNewFrame(lastCaptureFrame, 1000))
        {
            reportStatistics();
            continue;
        }

        const unsigned long long newestFrame = capture.GetFrameCount();
        if (newestFrame == lastCaptureFrame)
        {
            reportStatistics();
            continue;
        }
        const unsigned long long newlyCaptured = newestFrame - lastCaptureFrame;
        capturedFrames += newlyCaptured;
        if (newlyCaptured > 1)
        {
            skippedCaptureFrames += newlyCaptured - 1;
        }
        lastCaptureFrame = newestFrame;

        const NetworkServerStats currentNetworkState = server.GetStats();
        if (currentNetworkState.keyFrameInFlight)
        {
            // Do not feed frames to the encoder while its recovery IDR is still
            // queued/sending. This keeps the next P-frame reference chain intact.
            ++networkWaitSkippedFrames;
            reportStatistics();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < nextEncodeTime)
        {
            ++fpsLimitSkippedFrames;
            reportStatistics();
            continue;
        }
        nextEncodeTime = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / adaptiveTargetFps));

        if (!capture.GetLatestFrame(
            bgraTexture,
            width,
            height,
            captureTime100ns))
        {
            reportStatistics();
            continue;
        }

        std::vector<unsigned char> h264Frame;
        const auto encodeStart = std::chrono::steady_clock::now();
        const bool encodeSucceeded = encoder.EncodeFrame(
            bgraTexture.Get(),
            width,
            height,
            captureTime100ns,
            h264Frame);
        const double currentEncodeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - encodeStart
        ).count();
        ++encodeAttempts;
        encodeTimeMs += currentEncodeMs;
        maxEncodeTimeMs = std::max(maxEncodeTimeMs, currentEncodeMs);
        const H264EncoderTiming encoderTiming = encoder.GetLastTiming();
        conversionTimeMs += encoderTiming.conversionMs;
        waitForInputTimeMs += encoderTiming.waitForInputMs;
        sampleSetupTimeMs += encoderTiming.sampleSetupMs;
        processInputTimeMs += encoderTiming.processInputMs;
        waitForOutputTimeMs += encoderTiming.waitForOutputMs;
        processOutputTimeMs += encoderTiming.processOutputMs;

        if (!encodeSucceeded)
        {
            reportStatistics();
            continue;
        }

        if (h264Frame.empty())
        {
            reportStatistics();
            continue;
        }
        lastH264Size = h264Frame.size();
        encodedBytes += static_cast<double>(h264Frame.size());

        const auto enqueueStart = std::chrono::steady_clock::now();
        if (!server.SendH264Frame(std::move(h264Frame), width, height))
        {
            std::cout << "Viewer disconnected.\n";
            break;
        }
        const double currentEnqueueMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - enqueueStart
        ).count();
        enqueueTimeMs += currentEnqueueMs;
        maxEnqueueTimeMs = std::max(maxEnqueueTimeMs, currentEnqueueMs);

        ++submittedFrames;

        // Congestion is detected by the network worker, but all MFT calls stay
        // on this encoder thread. The queue only requests once it needs an IDR.
        if (server.ConsumeKeyFrameRequest())
        {
            encoder.RequestKeyFrame();
        }

        reportStatistics();
    }

    server.Stop();
    encoder.Shutdown();
    capture.Stop();
    MFShutdown();
    return 0;
}
