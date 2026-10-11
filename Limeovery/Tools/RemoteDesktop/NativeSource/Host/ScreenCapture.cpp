#include "ScreenCapture.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <chrono>
#include <iostream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    void LogHRESULT(const char* operation, HRESULT hr)
    {
        std::cout << operation << " failed: 0x"
            << std::hex << static_cast<unsigned long>(hr)
            << std::dec << "\n";
    }
}

bool ScreenCapture::Initialize()
{
    std::cout << "Creating D3D11 device...\n";

    const D3D_FEATURE_LEVEL requestedLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    D3D_FEATURE_LEVEL createdLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        requestedLevels,
        ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        &device,
        &createdLevel,
        &context
    );

    // Older Windows SDK/runtime combinations reject feature level 11_1 with
    // E_INVALIDARG. Retry with 11_0 while keeping the same hardware path.
    if (hr == E_INVALIDARG)
    {
        const D3D_FEATURE_LEVEL fallbackLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0
        };

        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            fallbackLevels,
            ARRAYSIZE(fallbackLevels),
            D3D11_SDK_VERSION,
            &device,
            &createdLevel,
            &context
        );
    }

    if (FAILED(hr))
    {
        LogHRESULT("D3D11CreateDevice", hr);
        return false;
    }

    // FrameArrived runs on a worker thread, while the encoder uses this same
    // immediate context for VideoProcessorBlt. Enable D3D11's context guard.
    hr = context.As(&multithread);
    if (FAILED(hr))
    {
        LogHRESULT("Query ID3D11Multithread", hr);
        return false;
    }
    multithread->SetMultithreadProtected(TRUE);

    std::cout << "D3D11 device created.\n";

    const HWND consoleWindow = GetConsoleWindow();
    const HMONITOR monitor = MonitorFromWindow(
        consoleWindow,
        MONITOR_DEFAULTTOPRIMARY
    );

    if (!monitor)
    {
        std::cout << "Failed to get monitor.\n";
        return false;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        std::cout << "GetMonitorInfoW failed.\n";
        return false;
    }

    std::cout << "Monitor resolution: "
        << monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left
        << "x"
        << monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top
        << "\n";

    using GraphicsCaptureItem =
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem;

    const auto interopFactory = winrt::get_activation_factory<
        GraphicsCaptureItem,
        IGraphicsCaptureItemInterop
    >();

    hr = interopFactory->CreateForMonitor(
        monitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(captureItem)
    );

    if (FAILED(hr))
    {
        LogHRESULT("CreateForMonitor", hr);
        return false;
    }

    std::cout << "GraphicsCaptureItem created.\n";
    return true;
}

bool ScreenCapture::Start()
{
    std::lock_guard lock(captureMutex);

    if (running.load())
    {
        return true;
    }

    if (!device || !captureItem)
    {
        std::cout << "Capture is not initialized.\n";
        return false;
    }

    try
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device.As(&dxgiDevice);
        if (FAILED(hr))
        {
            LogHRESULT("Query IDXGIDevice", hr);
            return false;
        }

        using WinRTD3DDevice =
            winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
        WinRTD3DDevice winrtDevice{ nullptr };

        hr = CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(),
            reinterpret_cast<IInspectable**>(winrt::put_abi(winrtDevice))
        );
        if (FAILED(hr))
        {
            LogHRESULT("CreateDirect3D11DeviceFromDXGIDevice", hr);
            return false;
        }

        using FramePool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
        using PixelFormat =
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

        framePool = FramePool::CreateFreeThreaded(
            winrtDevice,
            PixelFormat::B8G8R8A8UIntNormalized,
            3,
            captureItem.Size()
        );

        session = framePool.CreateCaptureSession(captureItem);
        session.IsCursorCaptureEnabled(true);

        // IsBorderRequired is runtime-gated so the executable still runs on
        // Windows builds whose WinRT contract predates this property.
        // Windows can keep the border visible unless the app has the
        // graphicsCaptureWithoutBorder capability and user consent.
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsBorderRequired"))
        {
            try
            {
                session.IsBorderRequired(false);
                std::cout << "Capture border suppression requested.\n";
            }
            catch (const winrt::hresult_error& error)
            {
                std::cout << "Could not disable capture border: 0x"
                    << std::hex << static_cast<unsigned long>(error.code().value)
                    << std::dec << "\n";
            }
        }
        else
        {
            std::cout << "This Windows build does not support IsBorderRequired.\n";
        }

        running.store(true);
        frameArrivedToken = framePool.FrameArrived(
            [this](FramePool const&, winrt::Windows::Foundation::IInspectable const&)
            {
                OnFrameArrived();
            }
        );

        std::cout << "Capture session created.\nStarting capture...\n";
        session.StartCapture();
        std::cout << "Capture started.\n";
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        running.store(false);
        std::cout << "Start failed: 0x"
            << std::hex << static_cast<unsigned long>(error.code().value)
            << std::dec << "\n";
        session = nullptr;
        framePool = nullptr;
        return false;
    }
}

void ScreenCapture::OnFrameArrived()
{
    std::lock_guard lock(captureMutex);
    if (!running.load() || !framePool)
    {
        return;
    }

    try
    {
        // Drain every queued frame so a slow callback never leaves the frame
        // pool backed up. Only the newest image is kept for a low-latency stream.
        while (auto frame = framePool.TryGetNextFrame())
        {
            const auto contentSize = frame.ContentSize();
            if (contentSize.Width <= 0 || contentSize.Height <= 0)
            {
                continue;
            }

            const auto surface = frame.Surface();
            using DXGIInterfaceAccess =
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

            const auto access = surface.as<DXGIInterfaceAccess>();
            Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
            HRESULT hr = access->GetInterface(IID_PPV_ARGS(&sourceTexture));
            if (FAILED(hr))
            {
                LogHRESULT("GetInterface(ID3D11Texture2D)", hr);
                continue;
            }

            D3D11_TEXTURE2D_DESC sourceDesc{};
            sourceTexture->GetDesc(&sourceDesc);

            if (!latestTexture ||
                sourceDesc.Width != frameWidth ||
                sourceDesc.Height != frameHeight)
            {
                latestTexture.Reset();
                consumerTexture.Reset();

                D3D11_TEXTURE2D_DESC ownedDesc = sourceDesc;
                ownedDesc.Usage = D3D11_USAGE_DEFAULT;
                ownedDesc.BindFlags = 0;
                ownedDesc.CPUAccessFlags = 0;
                ownedDesc.MiscFlags = 0;
                ownedDesc.ArraySize = 1;
                ownedDesc.MipLevels = 1;
                ownedDesc.SampleDesc.Count = 1;
                ownedDesc.SampleDesc.Quality = 0;

                hr = device->CreateTexture2D(&ownedDesc, nullptr, &latestTexture);
                if (FAILED(hr))
                {
                    LogHRESULT("CreateTexture2D(latest frame)", hr);
                    continue;
                }

                hr = device->CreateTexture2D(&ownedDesc, nullptr, &consumerTexture);
                if (FAILED(hr))
                {
                    latestTexture.Reset();
                    LogHRESULT("CreateTexture2D(consumer snapshot)", hr);
                    continue;
                }

                frameWidth = sourceDesc.Width;
                frameHeight = sourceDesc.Height;
                std::cout << "GPU frame buffers created: "
                    << frameWidth << "x" << frameHeight << "\n";
            }

            // GPU-to-GPU copy only; no staging texture, Map, Flush, or CPU
            // memcpy is performed in the callback.
            context->CopyResource(latestTexture.Get(), sourceTexture.Get());
            latestSystemRelativeTime100ns = frame.SystemRelativeTime().count();
            ++frameCount;
        }
    }
    catch (const winrt::hresult_error& error)
    {
        std::cout << "FrameArrived WinRT error: 0x"
            << std::hex << static_cast<unsigned long>(error.code().value)
            << std::dec << "\n";
    }

    frameArrivedCondition.notify_all();
}

bool ScreenCapture::GetLatestFrame(
    Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
    UINT& width,
    UINT& height,
    LONGLONG& systemRelativeTime100ns)
{
    std::lock_guard lock(captureMutex);
    if (!latestTexture || !consumerTexture)
    {
        return false;
    }

    // Snapshot the producer texture so FrameArrived can keep draining and
    // updating the frame pool while the encoder consumes a stable resource.
    context->CopyResource(consumerTexture.Get(), latestTexture.Get());
    texture = consumerTexture;
    width = frameWidth;
    height = frameHeight;
    systemRelativeTime100ns = latestSystemRelativeTime100ns;
    return true;
}

bool ScreenCapture::WaitForNewFrame(
    unsigned long long previousFrameCount,
    DWORD timeoutMs)
{
    std::unique_lock lock(captureMutex);
    return frameArrivedCondition.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMs),
        [this, previousFrameCount]
        {
            return !running.load() || frameCount.load() != previousFrameCount;
        }
    ) && running.load();
}

ID3D11Device* ScreenCapture::GetDevice() const noexcept
{
    return device.Get();
}

unsigned long long ScreenCapture::GetFrameCount() const noexcept
{
    return frameCount.load();
}

void ScreenCapture::Stop()
{
    running.store(false);
    frameArrivedCondition.notify_all();

    // Detach before taking captureMutex: an already-running callback may be
    // waiting for that lock and must be allowed to observe running == false.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{ nullptr };
    winrt::event_token token{};
    {
        std::lock_guard lock(captureMutex);
        pool = framePool;
        token = frameArrivedToken;
        frameArrivedToken = {};
    }

    if (pool)
    {
        try
        {
            pool.FrameArrived(token);
        }
        catch (...)
        {
        }
    }

    std::lock_guard lock(captureMutex);
    session = nullptr;
    framePool = nullptr;
    latestTexture.Reset();
    consumerTexture.Reset();
    frameWidth = 0;
    frameHeight = 0;

    std::cout << "Capture stopped.\n";
}
