#pragma once

#include <windows.h>

#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <wrl/client.h>

#include <vector>

class H264Decoder
{
public:
    bool Initialize(
        UINT width,
        UINT height,
        UINT fps
    );

    bool DecodeFrame(
        const std::vector<unsigned char>& h264,
        std::vector<unsigned char>& nv12
    );

    void Shutdown();

private:
    bool CreateInputSample(
        const std::vector<unsigned char>& data,
        IMFSample** sample
    );

    bool GetDecodedOutput(
        std::vector<unsigned char>& nv12
    );

private:
    Microsoft::WRL::ComPtr<IMFTransform>
        decoder;

    UINT width = 0;
    UINT height = 0;
    UINT fps = 0;

    DWORD outputStreamFlags = 0;
    DWORD outputBufferSize = 0;

    LONGLONG frameIndex = 0;
};
