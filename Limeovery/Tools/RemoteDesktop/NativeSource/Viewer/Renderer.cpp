#include "Renderer.h"

#include <d3dcompiler.h>

#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

bool Renderer::Initialize(
    UINT requestedWidth,
    UINT requestedHeight)
{
    width =
        requestedWidth;

    height =
        requestedHeight;

    if (width == 0 ||
        height == 0)
    {
        return false;
    }

    HINSTANCE instance =
        GetModuleHandleW(
            nullptr
        );

    WNDCLASSW wc{};

    wc.lpfnWndProc =
        Renderer::WindowProc;

    wc.hInstance =
        instance;

    wc.lpszClassName =
        L"RemoteDesktopViewerWindow";

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW
        );

    RegisterClassW(
        &wc
    );

    RECT rect{
        0,
        0,
        static_cast<LONG>(width),
        static_cast<LONG>(height)
    };

    AdjustWindowRect(
        &rect,
        WS_OVERLAPPEDWINDOW,
        FALSE
    );

    hwnd =
        CreateWindowExW(
            0,
            wc.lpszClassName,
            L"Remote Desktop Viewer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            instance,
            this
        );

    if (!hwnd)
    {
        return false;
    }

    if (!CreateDevice())
    {
        return false;
    }

    if (!CreateShaders())
    {
        return false;
    }

    if (!CreateTexture())
    {
        return false;
    }

    if (!CreateRenderTarget())
    {
        return false;
    }

    ShowWindow(
        hwnd,
        SW_SHOW
    );

    UpdateWindow(
        hwnd
    );

    running =
        true;

    return true;
}

bool Renderer::CreateDevice()
{
    DXGI_SWAP_CHAIN_DESC swapChainDesc{};

    swapChainDesc.BufferDesc.Width =
        width;

    swapChainDesc.BufferDesc.Height =
        height;

    swapChainDesc.BufferDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    swapChainDesc.BufferDesc.RefreshRate.Numerator =
        60;

    swapChainDesc.BufferDesc.RefreshRate.Denominator =
        1;

    swapChainDesc.SampleDesc.Count =
        1;

    swapChainDesc.BufferUsage =
        DXGI_USAGE_RENDER_TARGET_OUTPUT;

    swapChainDesc.BufferCount =
        2;

    swapChainDesc.OutputWindow =
        hwnd;

    swapChainDesc.Windowed =
        TRUE;

    // Prefer the flip model and a waitable swap chain so the renderer keeps at
    // most one frame queued for presentation. Fall back for older drivers.
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    D3D_FEATURE_LEVEL featureLevel{};

    const UINT flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    auto createDeviceAndSwapChain = [&]()
    {
        HRESULT createHr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &swapChain,
            &device,
            &featureLevel,
            &context);

        if (createHr == E_INVALIDARG)
        {
            swapChain.Reset();
            device.Reset();
            context.Reset();
            const D3D_FEATURE_LEVEL fallbackLevels[] =
            {
                D3D_FEATURE_LEVEL_11_0
            };
            createHr = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                fallbackLevels,
                ARRAYSIZE(fallbackLevels),
                D3D11_SDK_VERSION,
                &swapChainDesc,
                &swapChain,
                &device,
                &featureLevel,
                &context);
        }
        return createHr;
    };

    HRESULT hr = createDeviceAndSwapChain();

    if (FAILED(hr))
    {
        swapChain.Reset();
        device.Reset();
        context.Reset();
        swapChainDesc.Flags = 0;
        hr = createDeviceAndSwapChain();
    }

    if (FAILED(hr))
    {
        swapChain.Reset();
        device.Reset();
        context.Reset();
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = createDeviceAndSwapChain();
    }

    if (SUCCEEDED(hr) &&
        swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        SUCCEEDED(swapChain.As(&lowLatencySwapChain)))
    {
        if (SUCCEEDED(lowLatencySwapChain->SetMaximumFrameLatency(1)))
        {
            frameLatencyWaitableObject =
                lowLatencySwapChain->GetFrameLatencyWaitableObject();
        }
    }

    return SUCCEEDED(hr);
}

bool Renderer::CreateShaders()
{
    const char* vertexShaderCode = R"(
        struct VSOut
        {
            float4 position : SV_Position;
            float2 uv : TEXCOORD0;
        };

        VSOut main(uint vertexId : SV_VertexID)
        {
            VSOut output;

            float2 positions[3] =
            {
                float2(-1.0, -1.0),
                float2(-1.0,  3.0),
                float2( 3.0, -1.0)
            };

            float2 uvs[3] =
            {
                float2(0.0, 1.0),
                float2(0.0, -1.0),
                float2(2.0, 1.0)
            };

            output.position =
                float4(
                    positions[vertexId],
                    0.0,
                    1.0
                );

            output.uv =
                uvs[vertexId];

            return output;
        }
    )";

    const char* pixelShaderCode = R"(
        Texture2D lumaTexture : register(t0);
        Texture2D chromaTexture : register(t1);
        SamplerState frameSampler : register(s0);

        float4 main(
            float4 position : SV_Position,
            float2 uv : TEXCOORD0
        ) : SV_Target
        {
            // Input is limited-range BT.601 NV12, matching the previous CPU
            // converter. Keep the full-resolution YUV-to-RGB conversion on GPU.
            float y = lumaTexture.Sample(frameSampler, uv).r * 255.0 - 16.0;
            float2 chroma = chromaTexture.Sample(frameSampler, uv).rg * 255.0 - 128.0;
            float u = chroma.x;
            float v = chroma.y;

            float r = (298.0 * y + 409.0 * v + 128.0) / 256.0;
            float g = (298.0 * y - 100.0 * u - 208.0 * v + 128.0) / 256.0;
            float b = (298.0 * y + 516.0 * u + 128.0) / 256.0;

            return saturate(float4(r, g, b, 255.0) / 255.0);
        }
    )";

    Microsoft::WRL::ComPtr<ID3DBlob>
        vertexBlob;

    Microsoft::WRL::ComPtr<ID3DBlob>
        pixelBlob;

    Microsoft::WRL::ComPtr<ID3DBlob>
        errorBlob;

    HRESULT hr =
        D3DCompile(
            vertexShaderCode,
            std::strlen(vertexShaderCode),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "vs_5_0",
            0,
            0,
            &vertexBlob,
            &errorBlob
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        device->CreateVertexShader(
            vertexBlob->GetBufferPointer(),
            vertexBlob->GetBufferSize(),
            nullptr,
            &vertexShader
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        D3DCompile(
            pixelShaderCode,
            std::strlen(pixelShaderCode),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "ps_5_0",
            0,
            0,
            &pixelBlob,
            &errorBlob
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        device->CreatePixelShader(
            pixelBlob->GetBufferPointer(),
            pixelBlob->GetBufferSize(),
            nullptr,
            &pixelShader
        );

    return SUCCEEDED(hr);
}

bool Renderer::CreateTexture()
{
    if ((width & 1u) != 0 || (height & 1u) != 0)
    {
        return false;
    }

    auto createPlane = [this](
        UINT planeWidth,
        UINT planeHeight,
        DXGI_FORMAT format,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& planeTexture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& planeView)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = planeWidth;
        desc.Height = planeHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &planeTexture);
        if (FAILED(hr))
        {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(planeTexture.Get(), &srvDesc, &planeView);
        return SUCCEEDED(hr);
    };

    if (!createPlane(width, height, DXGI_FORMAT_R8_UNORM,
        lumaTexture, lumaShaderResourceView) ||
        !createPlane(width / 2, height / 2, DXGI_FORMAT_R8G8_UNORM,
            chromaTexture, chromaShaderResourceView))
    {
        return false;
    }

    D3D11_SAMPLER_DESC
        samplerDesc{};

    samplerDesc.Filter =
        D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    samplerDesc.AddressU =
        D3D11_TEXTURE_ADDRESS_CLAMP;

    samplerDesc.AddressV =
        D3D11_TEXTURE_ADDRESS_CLAMP;

    samplerDesc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;

    samplerDesc.MinLOD =
        0.0f;

    samplerDesc.MaxLOD =
        D3D11_FLOAT32_MAX;

    HRESULT hr =
        device->CreateSamplerState(
            &samplerDesc,
            &samplerState
        );

    return SUCCEEDED(hr);
}

bool Renderer::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        backBuffer;

    HRESULT hr =
        swapChain->GetBuffer(
            0,
            IID_PPV_ARGS(
                &backBuffer
            )
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        device->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            &renderTargetView
        );

    return SUCCEEDED(hr);
}

bool Renderer::UpdateFrame(
    const std::vector<unsigned char>& nv12)
{
    const size_t lumaSize = static_cast<size_t>(width) * height;
    const size_t expectedSize = lumaSize + lumaSize / 2;

    if (nv12.size() != expectedSize)
    {
        return false;
    }

    hasFrame = false;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(lumaTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        return false;
    }

    const auto* ySource = nv12.data();
    auto* yDestination = static_cast<unsigned char*>(mapped.pData);
    for (UINT row = 0; row < height; ++row)
    {
        std::memcpy(yDestination + static_cast<size_t>(row) * mapped.RowPitch,
            ySource + static_cast<size_t>(row) * width, width);
    }
    context->Unmap(lumaTexture.Get(), 0);

    hr = context->Map(chromaTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        return false;
    }

    const auto* uvSource = nv12.data() + lumaSize;
    auto* uvDestination = static_cast<unsigned char*>(mapped.pData);
    for (UINT row = 0; row < height / 2; ++row)
    {
        std::memcpy(uvDestination + static_cast<size_t>(row) * mapped.RowPitch,
            uvSource + static_cast<size_t>(row) * width, width);
    }
    context->Unmap(chromaTexture.Get(), 0);

    hasFrame = true;
    return true;
}

void Renderer::ProcessMessages()
{
    MSG message{};

    while (PeekMessageW(
        &message,
        nullptr,
        0,
        0,
        PM_REMOVE
    ))
    {
        if (message.message ==
            WM_QUIT)
        {
            running =
                false;

            return;
        }

        TranslateMessage(
            &message
        );

        DispatchMessageW(
            &message
        );
    }
}

void Renderer::Render()
{
    if (!context ||
        !renderTargetView)
    {
        return;
    }

    const float clearColor[4] =
    {
        0.0f,
        0.0f,
        0.0f,
        1.0f
    };

    context->OMSetRenderTargets(
        1,
        renderTargetView.GetAddressOf(),
        nullptr
    );

    context->ClearRenderTargetView(
        renderTargetView.Get(),
        clearColor
    );

    if (!hasFrame)
    {
        swapChain->Present(1, 0);
        return;
    }

    D3D11_VIEWPORT viewport{};

    viewport.Width =
        static_cast<float>(
            width
            );

    viewport.Height =
        static_cast<float>(
            height
            );

    viewport.MinDepth =
        0.0f;

    viewport.MaxDepth =
        1.0f;

    context->RSSetViewports(
        1,
        &viewport
    );

    context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    );

    context->VSSetShader(
        vertexShader.Get(),
        nullptr,
        0
    );

    context->PSSetShader(
        pixelShader.Get(),
        nullptr,
        0
    );

    ID3D11ShaderResourceView* srvs[] =
    {
        lumaShaderResourceView.Get(),
        chromaShaderResourceView.Get()
    };

    context->PSSetShaderResources(
        0,
        2,
        srvs
    );

    ID3D11SamplerState*
        sampler =
        samplerState.Get();

    context->PSSetSamplers(
        0,
        1,
        &sampler
    );

    context->Draw(
        3,
        0
    );

    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    context->PSSetShaderResources(0, 2, nullSrvs);

    swapChain->Present(
        1,
        0
    );
}

void Renderer::Run(
    const std::function<void()>& tick)
{
    while (running)
    {
        if (frameLatencyWaitableObject)
        {
            HANDLE handles[] = { frameLatencyWaitableObject };
            const DWORD waitResult = MsgWaitForMultipleObjectsEx(
                ARRAYSIZE(handles),
                handles,
                INFINITE,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
            if (waitResult == WAIT_FAILED)
            {
                frameLatencyWaitableObject = nullptr;
            }
            else if (waitResult == WAIT_OBJECT_0 + ARRAYSIZE(handles))
            {
                ProcessMessages();
                continue;
            }
            else if (waitResult != WAIT_OBJECT_0)
            {
                running = false;
            }
        }

        ProcessMessages();

        if (!running)
        {
            break;
        }

        if (tick)
        {
            tick();
        }

        Render();
    }
}

void Renderer::Shutdown()
{
    running =
        false;
    hasFrame = false;

    if (context)
    {
        context->ClearState();
        context->Flush();
    }

    renderTargetView.Reset();
    lumaShaderResourceView.Reset();
    chromaShaderResourceView.Reset();
    lumaTexture.Reset();
    chromaTexture.Reset();
    samplerState.Reset();
    lowLatencySwapChain.Reset();
    frameLatencyWaitableObject = nullptr;

    vertexShader.Reset();
    pixelShader.Reset();

    swapChain.Reset();
    context.Reset();
    device.Reset();

    if (hwnd)
    {
        DestroyWindow(
            hwnd
        );

        hwnd = nullptr;
    }
}

LRESULT CALLBACK Renderer::WindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message ==
        WM_NCCREATE)
    {
        auto* createStruct =
            reinterpret_cast<CREATESTRUCTW*>(
                lParam
                );

        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(
                createStruct->lpCreateParams
                )
        );
    }

    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(
            window
        );
        return 0;

    case WM_DESTROY:
        PostQuitMessage(
            0
        );
        return 0;

    default:
        break;
    }

    return DefWindowProcW(
        window,
        message,
        wParam,
        lParam
    );
}
