#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <mfapi.h>
#include <windows.h>

#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "NetworkClient.h"
#include "H264Decoder.h"
#include "Renderer.h"
#include "../Common/RemoteDesktopSecurity.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")

int main(int argc, char* argv[])
{
    const char* remoteAddress = "127.0.0.1";
    unsigned long remotePortValue = 5010;
    bool pinFromStandardInput = false;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--address") == 0)
        {
            remoteAddress = argv[++i];
        }
        else if (std::strcmp(argv[i], "--port") == 0)
        {
            char* end = nullptr;
            remotePortValue = std::strtoul(argv[++i], &end, 10);
            if (!end || *end != '\0' || remotePortValue == 0 || remotePortValue > 65535)
            {
                std::cerr << "Invalid --port value. Expected 1..65535.\n";
                return 2;
            }
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
        MessageBoxW(
            nullptr,
            L"Для защищённого подключения введите 12-значный PIN устройства в Limeovery.",
            L"Limeovery Viewer",
            MB_OK | MB_ICONERROR);
        return 2;
    }

    winrt::init_apartment(
        winrt::apartment_type::multi_threaded
    );

    std::cout
        << "=============================\n"
        << "      Remote Desktop Viewer\n"
        << "=============================\n\n";

    HRESULT hr =
        MFStartup(
            MF_VERSION
        );

    if (FAILED(hr))
    {
        return 1;
    }

    // ------------------------------------------------------------
    // Network
    // ------------------------------------------------------------

    NetworkClient client;

    if (!client.Connect(
        remoteAddress,
        static_cast<unsigned short>(remotePortValue),
        devicePin
    ))
    {
        SecureZeroMemory(devicePin.data(), devicePin.size());
        MessageBoxW(
            nullptr,
            L"Не удалось подключиться. Проверьте адрес, что Host запущен, и верность PIN.",
            L"Limeovery Viewer",
            MB_OK | MB_ICONERROR);
        MFShutdown();
        return 1;
    }
    SecureZeroMemory(devicePin.data(), devicePin.size());

    // ------------------------------------------------------------
    // First frame
    // ------------------------------------------------------------

    std::vector<unsigned char>
        firstFrame;

    unsigned int width = 0;
    unsigned int height = 0;

    std::cout
        << "Waiting for first frame...\n";

    if (!client.ReceiveH264Frame(
        firstFrame,
        width,
        height
    ))
    {
        client.Disconnect();
        MFShutdown();

        return 1;
    }

    std::cout
        << "Remote resolution: "
        << width
        << "x"
        << height
        << "\n";

    // ------------------------------------------------------------
    // Decoder
    // ------------------------------------------------------------

    H264Decoder decoder;

    if (!decoder.Initialize(
        width,
        height,
        120
    ))
    {
        client.Disconnect();
        MFShutdown();

        return 1;
    }

    // ------------------------------------------------------------
    // Renderer
    // ------------------------------------------------------------

    Renderer renderer;

    if (!renderer.Initialize(
        width,
        height
    ))
    {
        decoder.Shutdown();
        client.Disconnect();
        MFShutdown();

        return 1;
    }

    // ------------------------------------------------------------
    // Latest compressed frame
    // ------------------------------------------------------------

    std::mutex compressedMutex;

    std::condition_variable
        compressedCondition;

    struct CompressedFrame
    {
        std::vector<unsigned char> data;
        unsigned int width = 0;
        unsigned int height = 0;
    };

    std::deque<CompressedFrame> compressedFrames;
    // Keep at most two compressed access units ahead of the decoder. A larger
    // queue made the TCP receiver preserve stale frames and build visible lag.
    constexpr size_t compressedQueueCapacity = 2;

    // ------------------------------------------------------------
    // Latest decoded frame
    // ------------------------------------------------------------

    std::mutex decodedMutex;

    std::vector<unsigned char>
        latestDecodedFrame;

    bool hasDecodedFrame =
        false;

    // ------------------------------------------------------------
    // Global stop state
    // ------------------------------------------------------------

    std::atomic<bool>
        stopAll =
        false;

    // Queue the first frame before starting the network receiver.
    {
        std::lock_guard<std::mutex>
            lock(
                compressedMutex
            );

        compressedFrames.push_back(
            CompressedFrame{ std::move(firstFrame), width, height }
        );
    }

    compressedCondition.notify_one();

    // ------------------------------------------------------------
    // Network receiver thread
    // ------------------------------------------------------------

    std::thread networkThread(
        [&]()
        {
            winrt::init_apartment(
                winrt::apartment_type::multi_threaded
            );

            auto networkStatsStart = std::chrono::steady_clock::now();
            std::uint64_t receivedFramesInInterval = 0;

            while (!stopAll)
            {
                // Bound decode lead so the viewer cannot accumulate a long
                // backlog. The queue stays small to keep displayed content
                // close to live; the Host handles reference-chain recovery.
                {
                    std::unique_lock<std::mutex> lock(compressedMutex);
                    compressedCondition.wait(
                        lock,
                        [&]()
                        {
                            return stopAll || compressedFrames.size() < compressedQueueCapacity;
                        }
                    );
                    if (stopAll)
                    {
                        break;
                    }
                }

                std::vector<unsigned char>
                    receivedFrame;

                unsigned int
                    receivedWidth = 0;

                unsigned int
                    receivedHeight = 0;

                if (!client.ReceiveH264Frame(
                    receivedFrame,
                    receivedWidth,
                    receivedHeight
                ))
                {
                    break;
                }

                if (receivedWidth != width ||
                    receivedHeight != height)
                {
                    std::cout
                        << "Resolution change received.\n";

                    break;
                }

                ++receivedFramesInInterval;
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(
                    now - networkStatsStart).count();
                if (elapsed >= 2.0)
                {
                    std::cout << "[Viewer] Network receive: "
                        << (receivedFramesInInterval / elapsed) << " frames/s.\n";
                    receivedFramesInInterval = 0;
                    networkStatsStart = now;
                }

                {
                    std::lock_guard<std::mutex>
                        lock(
                            compressedMutex
                        );

                    compressedFrames.push_back(
                        CompressedFrame{
                            std::move(receivedFrame),
                            receivedWidth,
                            receivedHeight
                        }
                    );
                }

                compressedCondition.notify_one();
            }

            stopAll =
                true;

            compressedCondition.notify_all();
        }
    );

    // ------------------------------------------------------------
    // Decoder thread
    // ------------------------------------------------------------

    std::thread decoderThread(
        [&]()
        {
            winrt::init_apartment(
                winrt::apartment_type::multi_threaded
            );

            auto decodeStatsStart = std::chrono::steady_clock::now();
            std::uint64_t compressedFramesInInterval = 0;
            std::uint64_t decodedFramesInInterval = 0;

            while (true)
            {
                CompressedFrame compressedFrame;

                {
                    std::unique_lock<std::mutex>
                        lock(
                            compressedMutex
                        );

                    compressedCondition.wait(
                        lock,
                        [&]()
                        {
                            return
                                stopAll ||
                                !compressedFrames.empty();
                        }
                    );

                    if (stopAll &&
                        compressedFrames.empty())
                    {
                        break;
                    }

                    compressedFrame = std::move(compressedFrames.front());
                    compressedFrames.pop_front();
                }

                compressedCondition.notify_all();

                if (compressedFrame.data.empty())
                {
                    continue;
                }

                ++compressedFramesInInterval;

                if (compressedFrame.width != width ||
                    compressedFrame.height != height)
                {
                    continue;
                }

                std::vector<unsigned char>
                    decodedFrame;

                if (!decoder.DecodeFrame(
                    compressedFrame.data,
                    decodedFrame
                ))
                {
                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::chrono::duration<double>(
                        now - decodeStatsStart).count();
                    if (elapsed >= 2.0)
                    {
                        std::cout << "[Viewer] Decoder output: " << decodedFramesInInterval
                            << "/" << compressedFramesInInterval
                            << " frames in " << elapsed << " s.\n";
                        compressedFramesInInterval = 0;
                        decodedFramesInInterval = 0;
                        decodeStatsStart = now;
                    }
                    continue;
                }

                if (decodedFrame.empty())
                {
                    const auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::chrono::duration<double>(
                        now - decodeStatsStart).count();
                    if (elapsed >= 2.0)
                    {
                        std::cout << "[Viewer] Decoder output: " << decodedFramesInInterval
                            << "/" << compressedFramesInInterval
                            << " frames in " << elapsed << " s.\n";
                        compressedFramesInInterval = 0;
                        decodedFramesInInterval = 0;
                        decodeStatsStart = now;
                    }
                    continue;
                }

                ++decodedFramesInInterval;

                // ------------------------------------------------
                // Replace old decoded frame.
                // ------------------------------------------------

                {
                    std::lock_guard<std::mutex>
                        lock(
                            decodedMutex
                        );

                    latestDecodedFrame =
                        std::move(
                            decodedFrame
                        );

                    hasDecodedFrame =
                        true;
                }
            }
        }
    );

    std::cout
        << "\nRemote desktop stream started.\n";
    std::cout
        << "Close the Viewer window to exit.\n";

    // ------------------------------------------------------------
    // Render loop
    // ------------------------------------------------------------

    renderer.Run(
        [&]()
        {
            std::vector<unsigned char>
                frameToRender;

            {
                std::lock_guard<std::mutex>
                    lock(
                        decodedMutex
                    );

                if (!hasDecodedFrame)
                {
                    return;
                }

                frameToRender =
                    std::move(
                        latestDecodedFrame
                    );

                latestDecodedFrame.clear();

                hasDecodedFrame =
                    false;
            }

            if (!frameToRender.empty())
            {
                    if (!renderer.UpdateFrame(frameToRender))
                    {
                        static unsigned int uploadErrorsLogged = 0;
                        if (uploadErrorsLogged++ < 8)
                        {
                            std::cout << "[Viewer] Could not upload decoded NV12 frame to D3D11 textures.\n";
                        }
                    }
            }
        }
    );

    // ------------------------------------------------------------
    // Stop everything
    // ------------------------------------------------------------

    stopAll =
        true;

    client.Disconnect();

    compressedCondition.notify_all();

    if (networkThread.joinable())
    {
        networkThread.join();
    }

    if (decoderThread.joinable())
    {
        decoderThread.join();
    }

    // ------------------------------------------------------------
    // Cleanup
    // ------------------------------------------------------------

    renderer.Shutdown();
    decoder.Shutdown();

    MFShutdown();

    return 0;
}
