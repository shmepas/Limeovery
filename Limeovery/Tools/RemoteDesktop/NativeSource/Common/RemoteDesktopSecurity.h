#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RemoteDesktopSecurity
{
    constexpr std::size_t PinLength = 12;
    constexpr std::size_t GcmTagLength = 16;

    struct SessionKeys
    {
        std::array<unsigned char, 32> serverToClient{};
        std::array<unsigned char, 32> clientToServer{};
    };

    bool IsValidPin(const std::string& pin) noexcept;
    bool ClientHandshake(SOCKET socket, const std::string& pin, SessionKeys& keys);
    bool ServerHandshake(SOCKET socket, const std::string& pin, SessionKeys& keys);

    class AesGcmCipher
    {
    public:
        AesGcmCipher() = default;
        ~AesGcmCipher();

        AesGcmCipher(const AesGcmCipher&) = delete;
        AesGcmCipher& operator=(const AesGcmCipher&) = delete;

        bool Initialize(const std::array<unsigned char, 32>& key);
        bool Encrypt(
            std::uint64_t sequence,
            const unsigned char* additionalData,
            std::size_t additionalDataSize,
            const unsigned char* plaintext,
            std::size_t plaintextSize,
            std::vector<unsigned char>& ciphertextAndTag);
        bool Decrypt(
            std::uint64_t sequence,
            const unsigned char* additionalData,
            std::size_t additionalDataSize,
            const unsigned char* ciphertextAndTag,
            std::size_t ciphertextAndTagSize,
            std::vector<unsigned char>& plaintext);

    private:
        void Reset() noexcept;
        bool MakeNonce(std::uint64_t sequence, unsigned char nonce[12]) const noexcept;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE keyHandle = nullptr;
        std::vector<unsigned char> keyObject;
    };
}
