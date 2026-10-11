#pragma once

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <icodecapi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class H264EncoderEventQueue;

struct H264EncoderTiming
{
    double conversionMs = 0.0;
    double waitForInputMs = 0.0;
    double sampleSetupMs = 0.0;
    double processInputMs = 0.0;
    double waitForOutputMs = 0.0;
    double processOutputMs = 0.0;
};

class H264Encoder
{
public:
    bool Initialize(
        ID3D11Device* d3dDevice,
        UINT width,
        UINT height,
        UINT fps,
        UINT bitrate
    );

    bool RequestKeyFrame();
    bool SetBitrate(UINT newBitrate);
    H264EncoderTiming GetLastTiming() const noexcept;

    bool EncodeFrame(
        ID3D11Texture2D* bgraTexture,
        UINT frameWidth,
        UINT frameHeight,
        LONGLONG systemRelativeTime100ns,
        std::vector<unsigned char>& encodedFrame
    );

    void Shutdown();

private:
    bool InitializeVideoProcessor();
    bool ConvertTextureToNV12(ID3D11Texture2D* bgraTexture);
    bool CreateInputSample(
        LONGLONG systemRelativeTime100ns,
        IMFSample** sample
    );
    bool ProcessEncoderOutput(std::vector<unsigned char>& encodedFrame);
    bool ProcessOneOutput(std::vector<unsigned char>& encodedFrame);
    bool WaitForEvent(MediaEventType expectedType, DWORD timeoutMs);

private:
    Microsoft::WRL::ComPtr<IMFTransform> encoder;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    Microsoft::WRL::ComPtr<IMFSample> reusableOutputSample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> reusableOutputBuffer;
    std::shared_ptr<H264EncoderEventQueue> asyncEventQueue;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager;

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> videoProcessorEnumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> videoProcessorInputView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> videoProcessorInputTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> nv12OutputView;

    std::wstring encoderName;
    UINT width = 0;
    UINT height = 0;
    UINT fps = 0;
    UINT bitrate = 0;
    DWORD outputBufferSize = 0;
    DWORD outputStreamFlags = 0;
    std::uint64_t frameIndex = 0;
    LONGLONG firstCaptureTime100ns = 0;
    LONGLONG lastSampleTime100ns = -1;
    bool d3d11Aware = false;
    H264EncoderTiming lastTiming{};
};
