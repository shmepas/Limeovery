#pragma once

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

class ScreenCapture
{
public:
    bool Initialize();
    bool Start();
    void Stop();

    // Returns a GPU-resident snapshot. The returned texture remains valid
    // while the encoder consumes it and is not backed by a CPU readback.
    bool GetLatestFrame(
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        UINT& width,
        UINT& height,
        LONGLONG& systemRelativeTime100ns
    );

    bool WaitForNewFrame(
        unsigned long long previousFrameCount,
        DWORD timeoutMs
    );

    ID3D11Device* GetDevice() const noexcept;
    unsigned long long GetFrameCount() const noexcept;

private:
    void OnFrameArrived();

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };
    winrt::event_token frameArrivedToken{};

    // latestTexture is written by FrameArrived; consumerTexture is a stable
    // snapshot copied by GetLatestFrame and read by the encoder.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latestTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> consumerTexture;
    UINT frameWidth = 0;
    UINT frameHeight = 0;
    LONGLONG latestSystemRelativeTime100ns = 0;

    std::atomic<unsigned long long> frameCount{ 0 };
    std::atomic<bool> running{ false };
    mutable std::mutex captureMutex;
    std::condition_variable frameArrivedCondition;
};
