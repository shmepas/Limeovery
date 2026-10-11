#include "H264Encoder.h"

#include <mferror.h>
#include <mfidl.h>
#include <codecapi.h>
#include <oleauto.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <wrl/implements.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "oleaut32.lib")

class H264EncoderEventQueue final :
    public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
        IMFAsyncCallback>
{
public:
    HRESULT RuntimeClassInitialize(IMFMediaEventGenerator* generator)
    {
        if (!generator)
        {
            return E_INVALIDARG;
        }
        eventGenerator = generator;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* asyncResult) override
    {
        Microsoft::WRL::ComPtr<IMFMediaEvent> event;
        HRESULT hr = eventGenerator->EndGetEvent(asyncResult, &event);
        MediaEventType eventType = MEUnknown;
        HRESULT eventStatus = S_OK;

        if (SUCCEEDED(hr) && event)
        {
            hr = event->GetType(&eventType);
        }
        if (SUCCEEDED(hr) && event)
        {
            hr = event->GetStatus(&eventStatus);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (FAILED(hr))
            {
                callbackError = hr;
            }
            else if (eventType == MEError || FAILED(eventStatus))
            {
                callbackError = FAILED(eventStatus) ? eventStatus : E_FAIL;
            }
            else
            {
                try
                {
                    events.push_back(eventType);
                }
                catch (...)
                {
                    callbackError = E_OUTOFMEMORY;
                }
            }
        }
        condition.notify_all();

        if (running.load())
        {
            const HRESULT beginHr = eventGenerator->BeginGetEvent(this, nullptr);
            if (FAILED(beginHr) && running.load())
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    callbackError = beginHr;
                }
                condition.notify_all();
            }
        }

        return S_OK;
    }

    HRESULT Start()
    {
        running.store(true);
        const HRESULT hr = eventGenerator->BeginGetEvent(this, nullptr);
        if (FAILED(hr))
        {
            running.store(false);
        }
        return hr;
    }

    void Stop()
    {
        running.store(false);
        condition.notify_all();
    }

    bool WaitForEvent(MediaEventType expectedType, DWORD timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto hasExpectedEvent = [this, expectedType]()
        {
            return std::find(events.begin(), events.end(), expectedType) != events.end();
        };

        if (!condition.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [&]()
            {
                return FAILED(callbackError) || hasExpectedEvent() || !running.load();
            }))
        {
            return false;
        }

        if (FAILED(callbackError))
        {
            return false;
        }

        const auto found = std::find(events.begin(), events.end(), expectedType);
        if (found == events.end())
        {
            return false;
        }

        events.erase(found);
        return true;
    }

private:
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> eventGenerator;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<MediaEventType> events;
    std::atomic<bool> running{ false };
    HRESULT callbackError = S_OK;
};

namespace
{
    constexpr LONGLONG HNS_PER_SECOND = 10'000'000;

    void LogHRESULT(const char* operation, HRESULT hr)
    {
        std::cout << operation << " failed: 0x"
            << std::hex << static_cast<unsigned long>(hr)
            << std::dec << "\n";
    }
}

bool H264Encoder::Initialize(
    ID3D11Device* sourceDevice,
    UINT requestedWidth,
    UINT requestedHeight,
    UINT requestedFps,
    UINT requestedBitrate)
{
    Shutdown();

    width = requestedWidth;
    height = requestedHeight;
    fps = requestedFps;
    bitrate = requestedBitrate;

    if (!sourceDevice || width == 0 || height == 0 || fps == 0 ||
        (width & 1) != 0 || (height & 1) != 0)
    {
        std::cout << "Invalid encoder parameters or D3D device.\n";
        return false;
    }

    d3dDevice = sourceDevice;
    d3dDevice->GetImmediateContext(&d3dContext);
    if (!d3dContext)
    {
        std::cout << "Failed to get D3D11 immediate context.\n";
        return false;
    }

    std::cout << "Searching for hardware H.264 encoder...\n";

    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    IMFActivate** activations = nullptr;
    UINT32 activationCount = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &outputInfo,
        &activations,
        &activationCount
    );

    if (FAILED(hr))
    {
        LogHRESULT("MFTEnumEx", hr);
        return false;
    }

    if (activationCount == 0)
    {
        std::cout << "No hardware H.264 encoder found.\n";
        CoTaskMemFree(activations);
        return false;
    }

    std::cout << "Hardware H.264 encoders found: " << activationCount << "\n";

    Microsoft::WRL::ComPtr<IMFActivate> activate;
    activate = activations[0];
    for (UINT32 i = 0; i < activationCount; ++i)
    {
        activations[i]->Release();
    }
    CoTaskMemFree(activations);

    WCHAR* friendlyName = nullptr;
    UINT32 friendlyNameLength = 0;
    if (SUCCEEDED(activate->GetAllocatedString(
        MFT_FRIENDLY_NAME_Attribute,
        &friendlyName,
        &friendlyNameLength)))
    {
        encoderName = friendlyName;
        std::wcout << L"Selected encoder: " << encoderName << L"\n";
        CoTaskMemFree(friendlyName);
    }

    hr = activate->ActivateObject(IID_PPV_ARGS(&encoder));
    if (FAILED(hr))
    {
        LogHRESULT("ActivateObject", hr);
        return false;
    }

    // Force-keyframe support is optional in ICodecAPI. The encoder remains
    // usable if this particular hardware MFT does not expose that property.
    encoder.As(&codecApi);

    Microsoft::WRL::ComPtr<IMFAttributes> encoderAttributes;
    hr = encoder->GetAttributes(&encoderAttributes);
    if (FAILED(hr))
    {
        LogHRESULT("IMFTransform::GetAttributes", hr);
        return false;
    }

    const UINT32 isAsync = MFGetAttributeUINT32(
        encoderAttributes.Get(),
        MF_TRANSFORM_ASYNC,
        FALSE
    );
    std::cout << "Asynchronous MFT: " << (isAsync ? "YES" : "NO") << "\n";

    if (isAsync)
    {
        hr = encoderAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(hr))
        {
            LogHRESULT("Unlock asynchronous MFT", hr);
            return false;
        }
        std::cout << "Async MFT unlocked.\n";
    }

    const bool transformD3D11Aware = MFGetAttributeUINT32(
        encoderAttributes.Get(),
        MF_SA_D3D11_AWARE,
        FALSE
    ) != FALSE;

    if (transformD3D11Aware)
    {
        UINT resetToken = 0;
        hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiDeviceManager);
        if (FAILED(hr))
        {
            LogHRESULT("MFCreateDXGIDeviceManager", hr);
        }
        else
        {
            hr = dxgiDeviceManager->ResetDevice(d3dDevice.Get(), resetToken);
            if (SUCCEEDED(hr))
            {
                hr = encoder->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(dxgiDeviceManager.Get())
                );
            }

            if (SUCCEEDED(hr))
            {
                d3d11Aware = true;
                std::cout << "D3D11 zero-copy input enabled.\n";
            }
            else
            {
                LogHRESULT("Attach D3D11 device manager", hr);
                encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
                dxgiDeviceManager.Reset();
            }
        }
    }

    if (!d3d11Aware)
    {
        std::cout << "Selected H.264 MFT cannot accept D3D11 surfaces; "
            << "strict zero-copy mode requires a D3D11-aware encoder.\n";
        return false;
    }

    if (!InitializeVideoProcessor())
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr))
    {
        LogHRESULT("MFCreateMediaType(output)", hr);
        return false;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    outputType->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = encoder->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr))
    {
        LogHRESULT("SetOutputType(H264)", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr))
    {
        LogHRESULT("MFCreateMediaType(input)", hr);
        return false;
    }

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    inputType->SetUINT32(
        MF_MT_SAMPLE_SIZE,
        static_cast<UINT32>(static_cast<size_t>(width) * height * 3 / 2)
    );
    inputType->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = encoder->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr))
    {
        LogHRESULT("SetInputType(NV12)", hr);
        return false;
    }

    if (codecApi)
    {
        auto setOptionalBool = [this](const GUID& property, const wchar_t* name)
        {
            if (codecApi->IsSupported(&property) != S_OK)
            {
                std::wcout << L"Encoder does not expose " << name << L".\n";
                return;
            }

            VARIANT value;
            VariantInit(&value);
            value.vt = VT_BOOL;
            value.boolVal = VARIANT_TRUE;
            const HRESULT setHr = codecApi->SetValue(&property, &value);
            VariantClear(&value);
            if (SUCCEEDED(setHr))
            {
                std::wcout << name << L" enabled.\n";
            }
            else
            {
                std::wcout << L"Could not enable " << name << L": 0x"
                    << std::hex << static_cast<unsigned long>(setHr) << std::dec << L"\n";
            }
        };

        setOptionalBool(CODECAPI_AVEncCommonLowLatency, L"Encoder low-latency mode");
        setOptionalBool(CODECAPI_AVEncCommonRealTime, L"Real-time encoding mode");

        if (codecApi->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount) == S_OK)
        {
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_UI4;
            value.ulVal = 0;
            const HRESULT setHr = codecApi->SetValue(
                &CODECAPI_AVEncMPVDefaultBPictureCount,
                &value);
            VariantClear(&value);
            if (SUCCEEDED(setHr))
            {
                std::cout << "B-frame count set to 0 for low latency.\n";
            }
            else
            {
                LogHRESULT("Set B-frame count to 0", setHr);
            }
        }
        else
        {
            std::cout << "Encoder does not expose B-frame count control.\n";
        }
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = encoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr))
    {
        LogHRESULT("GetOutputStreamInfo", hr);
        return false;
    }

    outputStreamFlags = streamInfo.dwFlags;
    outputBufferSize = std::max<DWORD>(streamInfo.cbSize, 1024 * 1024);
    std::cout << "Output buffer size: " << outputBufferSize << " bytes\n";

    if ((outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
    {
        hr = MFCreateMemoryBuffer(outputBufferSize, &reusableOutputBuffer);
        if (FAILED(hr))
        {
            LogHRESULT("MFCreateMemoryBuffer(reusable output)", hr);
            return false;
        }

        hr = MFCreateSample(&reusableOutputSample);
        if (FAILED(hr))
        {
            LogHRESULT("MFCreateSample(reusable output)", hr);
            return false;
        }

        hr = reusableOutputSample->AddBuffer(reusableOutputBuffer.Get());
        if (FAILED(hr))
        {
            LogHRESULT("AddBuffer(reusable output)", hr);
            return false;
        }
    }

    if (isAsync)
    {
        hr = encoder.As(&eventGenerator);
        if (FAILED(hr))
        {
            LogHRESULT("Query IMFMediaEventGenerator", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<H264EncoderEventQueue> callback;
        hr = Microsoft::WRL::MakeAndInitialize<H264EncoderEventQueue>(
            &callback,
            eventGenerator.Get()
        );
        if (FAILED(hr))
        {
            LogHRESULT("Create asynchronous encoder event callback", hr);
            return false;
        }

        asyncEventQueue = std::shared_ptr<H264EncoderEventQueue>(
            callback.Detach(),
            [](H264EncoderEventQueue* queue)
            {
                if (queue)
                {
                    queue->Release();
                }
            }
        );
        hr = asyncEventQueue->Start();
        if (FAILED(hr))
        {
            LogHRESULT("IMFMediaEventGenerator::BeginGetEvent", hr);
            asyncEventQueue->Stop();
            asyncEventQueue.reset();
            return false;
        }
        std::cout << "Async event generator created.\n";
    }

    hr = encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr))
    {
        LogHRESULT("MFT_MESSAGE_NOTIFY_BEGIN_STREAMING", hr);
        return false;
    }

    hr = encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
    {
        LogHRESULT("MFT_MESSAGE_NOTIFY_START_OF_STREAM", hr);
        return false;
    }

    frameIndex = 0;
    firstCaptureTime100ns = 0;
    lastSampleTime100ns = -1;

    std::cout << "H.264 encoder initialized.\n"
        << "Resolution: " << width << "x" << height << "\n"
        << "FPS: " << fps << "\n"
        << "Bitrate: " << bitrate << " bps\n";
    return true;
}

bool H264Encoder::InitializeVideoProcessor()
{
    HRESULT hr = d3dDevice.As(&videoDevice);
    if (FAILED(hr))
    {
        LogHRESULT("Query ID3D11VideoDevice", hr);
        return false;
    }

    hr = d3dContext.As(&videoContext);
    if (FAILED(hr))
    {
        LogHRESULT("Query ID3D11VideoContext", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate = { fps, 1 };
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputFrameRate = { fps, 1 };
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = videoDevice->CreateVideoProcessorEnumerator(
        &contentDesc,
        &videoProcessorEnumerator
    );
    if (FAILED(hr))
    {
        LogHRESULT("CreateVideoProcessorEnumerator", hr);
        return false;
    }

    UINT inputSupport = 0;
    hr = videoProcessorEnumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM,
        &inputSupport
    );
    if (FAILED(hr) || !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
    {
        LogHRESULT("CheckVideoProcessorFormat(BGRA input)", FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    UINT outputSupport = 0;
    hr = videoProcessorEnumerator->CheckVideoProcessorFormat(
        DXGI_FORMAT_NV12,
        &outputSupport
    );
    if (FAILED(hr) || !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
    {
        LogHRESULT("CheckVideoProcessorFormat(NV12 output)", FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    hr = videoDevice->CreateVideoProcessor(
        videoProcessorEnumerator.Get(),
        0,
        &videoProcessor
    );
    if (FAILED(hr))
    {
        LogHRESULT("CreateVideoProcessor", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC nv12Desc{};
    nv12Desc.Width = width;
    nv12Desc.Height = height;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

    hr = d3dDevice->CreateTexture2D(&nv12Desc, nullptr, &nv12Texture);
    if (FAILED(hr))
    {
        // Some D3D11 drivers expose the encoder through a media transform but
        // reject the newer video-encoder bind bit on video-processor targets.
        nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = d3dDevice->CreateTexture2D(&nv12Desc, nullptr, &nv12Texture);
    }
    if (FAILED(hr))
    {
        LogHRESULT("CreateTexture2D(NV12)", hr);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc{};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    hr = videoDevice->CreateVideoProcessorOutputView(
        nv12Texture.Get(),
        videoProcessorEnumerator.Get(),
        &outputViewDesc,
        &nv12OutputView
    );
    if (FAILED(hr))
    {
        LogHRESULT("CreateVideoProcessorOutputView", hr);
        return false;
    }

    std::cout << "GPU BGRA-to-NV12 conversion configured (no CPU input fallback).\n";
    return true;
}

bool H264Encoder::ConvertTextureToNV12(ID3D11Texture2D* bgraTexture)
{
    if (!bgraTexture || !videoProcessor || !nv12Texture)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    bgraTexture->GetDesc(&sourceDesc);
    if (sourceDesc.Width != width || sourceDesc.Height != height ||
        sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        std::cout << "Capture texture format or dimensions changed; encoder needs reinitialization.\n";
        return false;
    }

    if (videoProcessorInputTexture.Get() != bgraTexture || !videoProcessorInputView)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc{};
        inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputViewDesc.Texture2D.MipSlice = 0;
        inputViewDesc.Texture2D.ArraySlice = 0;

        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
        const HRESULT createHr = videoDevice->CreateVideoProcessorInputView(
            bgraTexture,
            videoProcessorEnumerator.Get(),
            &inputViewDesc,
            &inputView
        );
        if (FAILED(createHr))
        {
            LogHRESULT("CreateVideoProcessorInputView", createHr);
            return false;
        }

        videoProcessorInputView = std::move(inputView);
        videoProcessorInputTexture = bgraTexture;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = videoProcessorInputView.Get();

    const HRESULT hr = videoContext->VideoProcessorBlt(
        videoProcessor.Get(),
        nv12OutputView.Get(),
        0,
        1,
        &stream
    );
    if (FAILED(hr))
    {
        LogHRESULT("VideoProcessorBlt(BGRA->NV12)", hr);
        return false;
    }

    return true;
}

bool H264Encoder::CreateInputSample(
    LONGLONG captureTime100ns,
    IMFSample** sample)
{
    if (!sample)
    {
        return false;
    }
    *sample = nullptr;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D),
        nv12Texture.Get(),
        0,
        FALSE,
        &buffer
    );
    if (FAILED(hr))
    {
        LogHRESULT("MFCreateDXGISurfaceBuffer (strict zero-copy)", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> newSample;
    hr = MFCreateSample(&newSample);
    if (FAILED(hr))
    {
        LogHRESULT("MFCreateSample", hr);
        return false;
    }

    hr = newSample->AddBuffer(buffer.Get());
    if (FAILED(hr))
    {
        LogHRESULT("IMFSample::AddBuffer", hr);
        return false;
    }

    const LONGLONG nominalFrameDuration = HNS_PER_SECOND / fps;
    LONGLONG timestamp = 0;
    if (captureTime100ns > 0)
    {
        if (firstCaptureTime100ns == 0)
        {
            firstCaptureTime100ns = captureTime100ns;
        }
        timestamp = captureTime100ns - firstCaptureTime100ns;
    }
    else
    {
        timestamp = static_cast<LONGLONG>(frameIndex) * nominalFrameDuration;
    }

    if (timestamp <= lastSampleTime100ns)
    {
        timestamp = lastSampleTime100ns + 1;
    }

    LONGLONG frameDuration = nominalFrameDuration;
    if (lastSampleTime100ns >= 0)
    {
        // Timestamps come from GraphicsCapture and remain monotonic when the
        // adaptive frame cap changes. Match sample duration to that real cadence.
        frameDuration = std::clamp<LONGLONG>(
            timestamp - lastSampleTime100ns,
            HNS_PER_SECOND / 240,
            HNS_PER_SECOND / 15);
    }

    hr = newSample->SetSampleTime(timestamp);
    if (FAILED(hr))
    {
        LogHRESULT("IMFSample::SetSampleTime", hr);
        return false;
    }

    hr = newSample->SetSampleDuration(frameDuration);
    if (FAILED(hr))
    {
        LogHRESULT("IMFSample::SetSampleDuration", hr);
        return false;
    }

    lastSampleTime100ns = timestamp;
    *sample = newSample.Detach();
    return true;
}

bool H264Encoder::WaitForEvent(MediaEventType expectedType, DWORD timeoutMs)
{
    if (!asyncEventQueue)
    {
        return true;
    }
    if (!asyncEventQueue->WaitForEvent(expectedType, timeoutMs))
    {
        std::cout << "Timed out or failed waiting for encoder event "
            << static_cast<unsigned long>(expectedType) << ".\n";
        return false;
    }
    return true;
}

bool H264Encoder::ProcessOneOutput(std::vector<unsigned char>& encodedFrame)
{
    encodedFrame.clear();

    const bool providesSamples =
        (outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    MFT_OUTPUT_DATA_BUFFER outputData{};
    outputData.dwStreamID = 0;

    if (!providesSamples)
    {
        if (!reusableOutputSample || !reusableOutputBuffer)
        {
            std::cout << "Reusable encoder output buffer is unavailable.\n";
            return false;
        }

        HRESULT hr = reusableOutputBuffer->SetCurrentLength(0);
        if (FAILED(hr))
        {
            LogHRESULT("Reset reusable output buffer", hr);
            return false;
        }
        outputData.pSample = reusableOutputSample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = encoder->ProcessOutput(0, 1, &outputData, &status);

    Microsoft::WRL::ComPtr<IMFCollection> outputEvents;
    if (outputData.pEvents)
    {
        outputEvents.Attach(outputData.pEvents);
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        if (providesSamples && outputData.pSample)
        {
            outputData.pSample->Release();
        }
        std::cout << "Async MFT returned NEED_MORE_INPUT after HAVE_OUTPUT.\n";
        return false;
    }

    if (FAILED(hr))
    {
        if (providesSamples && outputData.pSample)
        {
            outputData.pSample->Release();
        }
        LogHRESULT("Encoder ProcessOutput", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> outputSample;
    if (providesSamples)
    {
        outputSample.Attach(outputData.pSample);
    }
    else
    {
        outputSample = reusableOutputSample;
    }

    if (!outputSample)
    {
        std::cout << "Encoder ProcessOutput returned no sample.\n";
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
    HRESULT readHr = S_OK;
    if (providesSamples)
    {
        readHr = outputSample->ConvertToContiguousBuffer(&outputBuffer);
        if (FAILED(readHr))
        {
            LogHRESULT("ConvertToContiguousBuffer", readHr);
            return false;
        }
    }
    else
    {
        outputBuffer = reusableOutputBuffer;
    }

    DWORD dataLength = 0;
    readHr = outputBuffer->GetCurrentLength(&dataLength);
    if (FAILED(readHr) || dataLength == 0)
    {
        if (FAILED(readHr))
        {
            LogHRESULT("GetCurrentLength(H264 output)", readHr);
        }
        return false;
    }

    BYTE* data = nullptr;
    readHr = outputBuffer->Lock(&data, nullptr, nullptr);
    if (FAILED(readHr))
    {
        LogHRESULT("Lock(H264 output)", readHr);
        return false;
    }

    encodedFrame.assign(data, data + dataLength);
    outputBuffer->Unlock();
    return true;
}

bool H264Encoder::ProcessEncoderOutput(std::vector<unsigned char>& encodedFrame)
{
    const auto waitStart = std::chrono::steady_clock::now();
    if (eventGenerator && !WaitForEvent(METransformHaveOutput, 500))
    {
        lastTiming.waitForOutputMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - waitStart).count();
        return false;
    }
    lastTiming.waitForOutputMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - waitStart).count();

    const auto outputStart = std::chrono::steady_clock::now();
    const bool succeeded = ProcessOneOutput(encodedFrame);
    lastTiming.processOutputMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - outputStart).count();
    return succeeded;
}

bool H264Encoder::RequestKeyFrame()
{
    if (!codecApi)
    {
        std::cout << "Encoder does not expose ICodecAPI; keyframe request unavailable.\n";
        return false;
    }

    HRESULT hr = codecApi->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame);
    if (hr != S_OK)
    {
        std::cout << "Encoder does not support CODECAPI_AVEncVideoForceKeyFrame.\n";
        return false;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 1;

    hr = codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
    VariantClear(&value);
    if (hr != S_OK)
    {
        LogHRESULT("ICodecAPI::SetValue(ForceKeyFrame)", hr);
        return false;
    }

    std::cout << "Encoder will force the next input frame to a keyframe.\n";
    return true;
}

bool H264Encoder::SetBitrate(UINT newBitrate)
{
    if (!codecApi || newBitrate == 0)
    {
        return false;
    }

    HRESULT hr = codecApi->IsSupported(&CODECAPI_AVEncCommonMeanBitRate);
    if (hr != S_OK)
    {
        std::cout << "Encoder does not support dynamic bitrate changes.\n";
        return false;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = newBitrate;
    hr = codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
    VariantClear(&value);
    if (hr != S_OK)
    {
        LogHRESULT("ICodecAPI::SetValue(MeanBitRate)", hr);
        return false;
    }

    bitrate = newBitrate;
    std::cout << "Encoder target bitrate updated to " << bitrate << " bps.\n";
    return true;
}

H264EncoderTiming H264Encoder::GetLastTiming() const noexcept
{
    return lastTiming;
}

bool H264Encoder::EncodeFrame(
    ID3D11Texture2D* bgraTexture,
    UINT frameWidth,
    UINT frameHeight,
    LONGLONG systemRelativeTime100ns,
    std::vector<unsigned char>& encodedFrame)
{
    encodedFrame.clear();
    lastTiming = {};
    if (!encoder || !bgraTexture || frameWidth != width || frameHeight != height)
    {
        return false;
    }

    auto stageStart = std::chrono::steady_clock::now();
    if (!ConvertTextureToNV12(bgraTexture))
    {
        return false;
    }
    lastTiming.conversionMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stageStart).count();

    stageStart = std::chrono::steady_clock::now();
    if (eventGenerator && !WaitForEvent(METransformNeedInput, 500))
    {
        lastTiming.waitForInputMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        return false;
    }
    lastTiming.waitForInputMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stageStart).count();

    Microsoft::WRL::ComPtr<IMFSample> inputSample;
    stageStart = std::chrono::steady_clock::now();
    if (!CreateInputSample(systemRelativeTime100ns, &inputSample))
    {
        lastTiming.sampleSetupMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        return false;
    }
    lastTiming.sampleSetupMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stageStart).count();

    stageStart = std::chrono::steady_clock::now();
    HRESULT hr = encoder->ProcessInput(0, inputSample.Get(), 0);
    lastTiming.processInputMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stageStart).count();
    if (hr == MF_E_NOTACCEPTING)
    {
        // Drain one completed output before returning to the caller. In a live
        // stream the just-captured input is stale by the time this happens, so
        // deliberately skip it rather than retaining a surface the MFT rejected.
        stageStart = std::chrono::steady_clock::now();
        const bool haveOutput = eventGenerator && WaitForEvent(METransformHaveOutput, 100);
        lastTiming.waitForOutputMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        if (haveOutput)
        {
            stageStart = std::chrono::steady_clock::now();
            const bool outputSucceeded = ProcessOneOutput(encodedFrame) && !encodedFrame.empty();
            lastTiming.processOutputMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stageStart).count();
            return outputSucceeded;
        }

        stageStart = std::chrono::steady_clock::now();
        const bool needInput = !eventGenerator || WaitForEvent(METransformNeedInput, 500);
        lastTiming.waitForInputMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        if (!needInput)
        {
            return false;
        }
        stageStart = std::chrono::steady_clock::now();
        hr = encoder->ProcessInput(0, inputSample.Get(), 0);
        lastTiming.processInputMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - stageStart).count();
        if (FAILED(hr))
        {
            LogHRESULT("Encoder ProcessInput retry", hr);
            return false;
        }
    }

    if (FAILED(hr))
    {
        LogHRESULT("Encoder ProcessInput", hr);
        return false;
    }

    ++frameIndex;
    return ProcessEncoderOutput(encodedFrame);
}

void H264Encoder::Shutdown()
{
    if (asyncEventQueue)
    {
        asyncEventQueue->Stop();
    }

    if (encoder)
    {
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (dxgiDeviceManager)
        {
            encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }

        Microsoft::WRL::ComPtr<IMFShutdown> mftShutdown;
        if (SUCCEEDED(encoder.As(&mftShutdown)))
        {
            mftShutdown->Shutdown();
        }
    }

    eventGenerator.Reset();
    codecApi.Reset();
    reusableOutputSample.Reset();
    reusableOutputBuffer.Reset();
    asyncEventQueue.reset();
    encoder.Reset();
    dxgiDeviceManager.Reset();
    nv12OutputView.Reset();
    nv12Texture.Reset();
    videoProcessor.Reset();
    videoProcessorInputView.Reset();
    videoProcessorInputTexture.Reset();
    videoProcessorEnumerator.Reset();
    videoContext.Reset();
    videoDevice.Reset();
    d3dContext.Reset();
    d3dDevice.Reset();

    width = 0;
    height = 0;
    fps = 0;
    bitrate = 0;
    outputBufferSize = 0;
    outputStreamFlags = 0;
    frameIndex = 0;
    firstCaptureTime100ns = 0;
    lastSampleTime100ns = -1;
    d3d11Aware = false;

    std::cout << "H.264 encoder shut down.\n";
}
