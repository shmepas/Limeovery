#include "H264Decoder.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>
#include <iostream>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

bool H264Decoder::Initialize(
    UINT requestedWidth,
    UINT requestedHeight,
    UINT requestedFps)
{
    width =
        requestedWidth;

    height =
        requestedHeight;

    fps =
        requestedFps;

    if (width == 0 ||
        height == 0 ||
        fps == 0)
    {
        return false;
    }

    std::cout
        << "Initializing H.264 decoder...\n";

    MFT_REGISTER_TYPE_INFO inputInfo{};

    inputInfo.guidMajorType =
        MFMediaType_Video;

    inputInfo.guidSubtype =
        MFVideoFormat_H264;

    IMFActivate** activates =
        nullptr;

    UINT32 activateCount =
        0;

    HRESULT hr =
        MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT |
            MFT_ENUM_FLAG_LOCALMFT |
            MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            nullptr,
            &activates,
            &activateCount
        );

    if (FAILED(hr))
    {
        std::cout
            << "MFTEnumEx failed: 0x"
            << std::hex
            << hr
            << std::dec
            << "\n";

        return false;
    }

    if (activateCount == 0)
    {
        CoTaskMemFree(activates);
        return false;
    }

    IMFActivate* selected =
        nullptr;

    for (UINT32 i = 0;
        i < activateCount;
        ++i)
    {
        UINT32 asyncValue =
            FALSE;

        bool async =
            false;

        if (SUCCEEDED(
            activates[i]->GetUINT32(
                MF_TRANSFORM_ASYNC,
                &asyncValue
            )))
        {
            async =
                asyncValue != FALSE;
        }

        if (!async)
        {
            selected =
                activates[i];

            selected->AddRef();

            break;
        }
    }

    for (UINT32 i = 0;
        i < activateCount;
        ++i)
    {
        activates[i]->Release();
    }

    CoTaskMemFree(
        activates
    );

    if (!selected)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFActivate>
        activate =
        selected;

    WCHAR* friendlyName =
        nullptr;

    UINT32 nameLength =
        0;

    if (SUCCEEDED(
        activate->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute,
            &friendlyName,
            &nameLength
        )))
    {
        std::wcout
            << L"Selected decoder: "
            << friendlyName
            << L"\n";

        CoTaskMemFree(
            friendlyName
        );
    }

    hr =
        activate->ActivateObject(
            IID_PPV_ARGS(
                &decoder
            )
        );

    if (FAILED(hr))
    {
        return false;
    }

    std::cout
        << "H.264 decoder created.\n";

    // ------------------------------------------------------------
    // H264 input
    // ------------------------------------------------------------

    Microsoft::WRL::ComPtr<IMFMediaType>
        inputType;

    hr =
        MFCreateMediaType(
            &inputType
        );

    if (FAILED(hr))
    {
        return false;
    }

    inputType->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Video
    );

    inputType->SetGUID(
        MF_MT_SUBTYPE,
        MFVideoFormat_H264
    );

    MFSetAttributeSize(
        inputType.Get(),
        MF_MT_FRAME_SIZE,
        width,
        height
    );

    MFSetAttributeRatio(
        inputType.Get(),
        MF_MT_FRAME_RATE,
        fps,
        1
    );

    MFSetAttributeRatio(
        inputType.Get(),
        MF_MT_PIXEL_ASPECT_RATIO,
        1,
        1
    );

    inputType->SetUINT32(
        MF_MT_INTERLACE_MODE,
        MFVideoInterlace_Progressive
    );

    inputType->SetUINT32(
        MF_LOW_LATENCY,
        TRUE
    );

    hr =
        decoder->SetInputType(
            0,
            inputType.Get(),
            0
        );

    if (FAILED(hr))
    {
        std::cout
            << "Decoder SetInputType failed: 0x"
            << std::hex
            << hr
            << std::dec
            << "\n";

        return false;
    }

    // ------------------------------------------------------------
    // NV12 output
    // ------------------------------------------------------------

    Microsoft::WRL::ComPtr<IMFMediaType>
        outputType;

    hr =
        MFCreateMediaType(
            &outputType
        );

    if (FAILED(hr))
    {
        return false;
    }

    outputType->SetGUID(
        MF_MT_MAJOR_TYPE,
        MFMediaType_Video
    );

    outputType->SetGUID(
        MF_MT_SUBTYPE,
        MFVideoFormat_NV12
    );

    MFSetAttributeSize(
        outputType.Get(),
        MF_MT_FRAME_SIZE,
        width,
        height
    );

    MFSetAttributeRatio(
        outputType.Get(),
        MF_MT_FRAME_RATE,
        fps,
        1
    );

    MFSetAttributeRatio(
        outputType.Get(),
        MF_MT_PIXEL_ASPECT_RATIO,
        1,
        1
    );

    outputType->SetUINT32(
        MF_MT_INTERLACE_MODE,
        MFVideoInterlace_Progressive
    );

    outputType->SetUINT32(
        MF_MT_FIXED_SIZE_SAMPLES,
        TRUE
    );

    outputType->SetUINT32(
        MF_MT_SAMPLE_SIZE,
        width * height * 3 / 2
    );

    outputType->SetUINT32(
        MF_LOW_LATENCY,
        TRUE
    );

    hr =
        decoder->SetOutputType(
            0,
            outputType.Get(),
            0
        );

    if (FAILED(hr))
    {
        std::cout
            << "Decoder SetOutputType failed: 0x"
            << std::hex
            << hr
            << std::dec
            << "\n";

        return false;
    }

    // ------------------------------------------------------------
    // Output stream
    // ------------------------------------------------------------

    MFT_OUTPUT_STREAM_INFO streamInfo{};

    hr =
        decoder->GetOutputStreamInfo(
            0,
            &streamInfo
        );

    if (FAILED(hr))
    {
        return false;
    }

    outputStreamFlags =
        streamInfo.dwFlags;

    outputBufferSize =
        streamInfo.cbSize;

    const DWORD minimumSize =
        width * height * 3 / 2;

    if (outputBufferSize <
        minimumSize)
    {
        outputBufferSize =
            minimumSize;
    }

    // ------------------------------------------------------------
    // Start
    // ------------------------------------------------------------

    hr =
        decoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
            0
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        decoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM,
            0
        );

    if (FAILED(hr))
    {
        return false;
    }

    frameIndex = 0;

    std::cout
        << "H.264 decoder initialized.\n";

    return true;
}

bool H264Decoder::CreateInputSample(
    const std::vector<unsigned char>& data,
    IMFSample** sample)
{
    if (!sample ||
        data.empty())
    {
        return false;
    }

    *sample = nullptr;

    Microsoft::WRL::ComPtr<IMFMediaBuffer>
        buffer;

    HRESULT hr =
        MFCreateMemoryBuffer(
            static_cast<DWORD>(
                data.size()
                ),
            &buffer
        );

    if (FAILED(hr))
    {
        return false;
    }

    BYTE* destination =
        nullptr;

    hr =
        buffer->Lock(
            &destination,
            nullptr,
            nullptr
        );

    if (FAILED(hr))
    {
        return false;
    }

    std::memcpy(
        destination,
        data.data(),
        data.size()
    );

    buffer->Unlock();

    hr =
        buffer->SetCurrentLength(
            static_cast<DWORD>(
                data.size()
                )
        );

    if (FAILED(hr))
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample>
        newSample;

    hr =
        MFCreateSample(
            &newSample
        );

    if (FAILED(hr))
    {
        return false;
    }

    hr =
        newSample->AddBuffer(
            buffer.Get()
        );

    if (FAILED(hr))
    {
        return false;
    }

    const LONGLONG frameDuration =
        10000000LL /
        fps;

    newSample->SetSampleTime(
        frameIndex *
        frameDuration
    );

    newSample->SetSampleDuration(
        frameDuration
    );

    *sample =
        newSample.Detach();

    return true;
}

bool H264Decoder::GetDecodedOutput(
    std::vector<unsigned char>& nv12)
{
    nv12.clear();
    const size_t expectedSize = static_cast<size_t>(width) * height * 3 / 2;
    bool producedFrame = false;

    // A synchronous decoder can have more than one output ready for an input
    // (and can require an output type renegotiation after it parses SPS/PPS).
    // Drain until NEED_MORE_INPUT instead of silently abandoning that output.
    for (unsigned int attempt = 0; attempt < 8; ++attempt)
    {
        const bool providesSamples =
            (outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        Microsoft::WRL::ComPtr<IMFSample> outputSample;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outputBuffer;
        MFT_OUTPUT_DATA_BUFFER outputData{};
        outputData.dwStreamID = 0;

        if (!providesSamples)
        {
            HRESULT createHr = MFCreateMemoryBuffer(outputBufferSize, &outputBuffer);
            if (SUCCEEDED(createHr))
            {
                createHr = MFCreateSample(&outputSample);
            }
            if (SUCCEEDED(createHr))
            {
                createHr = outputSample->AddBuffer(outputBuffer.Get());
            }
            if (FAILED(createHr))
            {
                std::cout << "Could not allocate decoder output sample: 0x"
                    << std::hex << static_cast<unsigned long>(createHr) << std::dec << "\n";
                return producedFrame;
            }
            outputData.pSample = outputSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = decoder->ProcessOutput(0, 1, &outputData, &status);

        auto releaseOutput = [&]()
        {
            if (outputData.pEvents)
            {
                outputData.pEvents->Release();
                outputData.pEvents = nullptr;
            }
            if (providesSamples && outputData.pSample)
            {
                outputData.pSample->Release();
                outputData.pSample = nullptr;
            }
        };

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            releaseOutput();
            bool selectedNv12 = false;
            for (DWORD typeIndex = 0; ; ++typeIndex)
            {
                Microsoft::WRL::ComPtr<IMFMediaType> availableType;
                const HRESULT typeHr = decoder->GetOutputAvailableType(
                    0, typeIndex, &availableType);
                if (typeHr == MF_E_NO_MORE_TYPES)
                {
                    break;
                }
                if (FAILED(typeHr))
                {
                    break;
                }

                GUID subtype{};
                UINT32 availableWidth = 0;
                UINT32 availableHeight = 0;
                if (FAILED(availableType->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                    subtype != MFVideoFormat_NV12 ||
                    FAILED(MFGetAttributeSize(availableType.Get(), MF_MT_FRAME_SIZE,
                        &availableWidth, &availableHeight)) ||
                    availableWidth != width || availableHeight != height)
                {
                    continue;
                }

                if (SUCCEEDED(decoder->SetOutputType(0, availableType.Get(), 0)))
                {
                    MFT_OUTPUT_STREAM_INFO streamInfo{};
                    if (SUCCEEDED(decoder->GetOutputStreamInfo(0, &streamInfo)))
                    {
                        outputStreamFlags = streamInfo.dwFlags;
                        outputBufferSize = static_cast<DWORD>(std::max<size_t>(
                            streamInfo.cbSize, expectedSize));
                        selectedNv12 = true;
                    }
                }
                break;
            }

            if (!selectedNv12)
            {
                std::cout << "Decoder requested an output stream change but did not offer "
                    << "NV12 at " << width << "x" << height << ".\n";
                return producedFrame;
            }
            std::cout << "Decoder output stream renegotiated to NV12.\n";
            continue;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            releaseOutput();
            if (!producedFrame)
            {
                static unsigned int needMoreInputLogged = 0;
                if (needMoreInputLogged++ < 4)
                {
                    std::cout << "Decoder has accepted H.264 input but needs another access unit before output.\n";
                }
            }
            return producedFrame;
        }

        if (FAILED(hr))
        {
            releaseOutput();
            static unsigned int outputErrorsLogged = 0;
            if (outputErrorsLogged++ < 8)
            {
                std::cout << "Decoder ProcessOutput failed: 0x"
                    << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
            }
            return producedFrame;
        }

        IMFSample* finalSample = outputData.pSample
            ? outputData.pSample
            : outputSample.Get();
        if (!finalSample)
        {
            releaseOutput();
            std::cout << "Decoder returned success without an output sample.\n";
            return producedFrame;
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguousBuffer;
        hr = finalSample->ConvertToContiguousBuffer(&contiguousBuffer);
        DWORD currentLength = 0;
        if (SUCCEEDED(hr))
        {
            hr = contiguousBuffer->GetCurrentLength(&currentLength);
        }
        if (FAILED(hr) || currentLength < expectedSize)
        {
            releaseOutput();
            static unsigned int shortOutputsLogged = 0;
            if (shortOutputsLogged++ < 8)
            {
                std::cout << "Decoder returned an incomplete NV12 sample (length="
                    << currentLength << ", expected at least " << expectedSize
                    << ", hr=0x" << std::hex << static_cast<unsigned long>(hr)
                    << std::dec << ").\n";
            }
            return producedFrame;
        }

        BYTE* data = nullptr;
        hr = contiguousBuffer->Lock(&data, nullptr, nullptr);
        if (FAILED(hr) || !data)
        {
            releaseOutput();
            std::cout << "Could not lock decoder output buffer: 0x"
                << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
            return producedFrame;
        }

        // This packed buffer is uploaded to the Viewer GPU textures; the
        // YUV-to-RGB conversion itself remains in the pixel shader.
        nv12.assign(data, data + expectedSize);
        contiguousBuffer->Unlock();
        producedFrame = true;
        releaseOutput();
    }

    return producedFrame;
}

bool H264Decoder::DecodeFrame(
    const std::vector<unsigned char>& h264,
    std::vector<unsigned char>& nv12)
{
    nv12.clear();

    if (!decoder)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample>
        inputSample;

    if (!CreateInputSample(
        h264,
        &inputSample
    ))
    {
        return false;
    }

    HRESULT hr = decoder->ProcessInput(0, inputSample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        // Retrieve a pending decoded frame before retrying input. Ignoring this
        // state causes a decoder to stop making progress and the Viewer to stay black.
        GetDecodedOutput(nv12);
        hr = decoder->ProcessInput(0, inputSample.Get(), 0);
    }

    if (FAILED(hr))
    {
        static unsigned int inputErrorsLogged = 0;
        if (inputErrorsLogged++ < 8)
        {
            std::cout << "Decoder ProcessInput failed: 0x"
                << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
        }
        return !nv12.empty();
    }

    ++frameIndex;

    // This decoder is enumerated as synchronous. Polling it for up to 20 ms
    // with Sleep(1) added a variable delay after every input packet. With the
    // host configured for a no-B-frame low-latency stream, output is available
    // immediately or the transform needs another input.
    return GetDecodedOutput(nv12);
}

void H264Decoder::Shutdown()
{
    if (decoder)
    {
        decoder->ProcessMessage(
            MFT_MESSAGE_COMMAND_FLUSH,
            0
        );

        decoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_END_OF_STREAM,
            0
        );

        decoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_END_STREAMING,
            0
        );
    }

    decoder.Reset();

    frameIndex = 0;
}
