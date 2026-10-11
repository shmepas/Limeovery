#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_3.h>

#include <wrl/client.h>

#include <functional>
#include <vector>

class Renderer
{
public:
    bool Initialize(
        UINT width,
        UINT height
    );

    bool UpdateFrame(
        const std::vector<unsigned char>& nv12
    );

    void Run(
        const std::function<void()>& tick
    );

    void Shutdown();

private:
    bool CreateDevice();
    bool CreateShaders();
    bool CreateTexture();
    bool CreateRenderTarget();

    void Render();
    void ProcessMessages();

    static LRESULT CALLBACK WindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam
    );

private:
    HWND hwnd = nullptr;

    UINT width = 0;
    UINT height = 0;

    bool running = false;
    bool hasFrame = false;

    Microsoft::WRL::ComPtr<ID3D11Device>
        device;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext>
        context;

    Microsoft::WRL::ComPtr<IDXGISwapChain>
        swapChain;

    Microsoft::WRL::ComPtr<IDXGISwapChain2>
        lowLatencySwapChain;

    HANDLE frameLatencyWaitableObject = nullptr;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
        renderTargetView;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>
        vertexShader;

    Microsoft::WRL::ComPtr<ID3D11PixelShader>
        pixelShader;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        lumaTexture;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>
        chromaTexture;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        lumaShaderResourceView;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        chromaShaderResourceView;

    Microsoft::WRL::ComPtr<ID3D11SamplerState>
        samplerState;
};
