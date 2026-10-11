#include "RemoteDesktopSecurity.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>

#pragma comment(lib, "bcrypt.lib")

namespace RemoteDesktopSecurity
{
    namespace
    {
        constexpr unsigned char ProtocolMagic[4] = { 'L', 'M', 'V', '2' };
        constexpr ULONG Pbkdf2Iterations = 300000;
        constexpr std::size_t NonceSize = 16;
        constexpr std::size_t SaltSize = 16;
        constexpr std::size_t ProofSize = 32;
        constexpr std::size_t KdfOutputSize = 96;

        struct ClientHello
        {
            unsigned char magic[4];
            unsigned char nonce[NonceSize];
        };

        struct ServerHello
        {
            unsigned char magic[4];
            unsigned char salt[SaltSize];
            unsigned char nonce[NonceSize];
        };

        bool SendAll(SOCKET socket, const unsigned char* data, std::size_t size)
        {
            std::size_t sentTotal = 0;
            while (sentTotal < size)
            {
                const std::size_t remaining = size - sentTotal;
                const int chunk = static_cast<int>(std::min<std::size_t>(
                    remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const int sent = send(
                    socket,
                    reinterpret_cast<const char*>(data + sentTotal),
                    chunk,
                    0);
                if (sent <= 0)
                {
                    return false;
                }
                sentTotal += static_cast<std::size_t>(sent);
            }
            return true;
        }

        bool ReceiveAll(SOCKET socket, unsigned char* data, std::size_t size)
        {
            std::size_t receivedTotal = 0;
            while (receivedTotal < size)
            {
                const std::size_t remaining = size - receivedTotal;
                const int chunk = static_cast<int>(std::min<std::size_t>(
                    remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const int received = recv(
                    socket,
                    reinterpret_cast<char*>(data + receivedTotal),
                    chunk,
                    0);
                if (received <= 0)
                {
                    return false;
                }
                receivedTotal += static_cast<std::size_t>(received);
            }
            return true;
        }

        bool RandomBytes(unsigned char* destination, std::size_t size)
        {
            return size <= ULONG_MAX &&
                BCRYPT_SUCCESS(BCryptGenRandom(
                    nullptr,
                    destination,
                    static_cast<ULONG>(size),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        }

        bool DeriveKeys(
            const std::string& pin,
            const ClientHello& clientHello,
            const ServerHello& serverHello,
            unsigned char output[KdfOutputSize])
        {
            unsigned char salt[SaltSize + NonceSize + NonceSize]{};
            std::memcpy(salt, serverHello.salt, SaltSize);
            std::memcpy(salt + SaltSize, clientHello.nonce, NonceSize);
            std::memcpy(salt + SaltSize + NonceSize, serverHello.nonce, NonceSize);

            BCRYPT_ALG_HANDLE hashAlgorithm = nullptr;
            const NTSTATUS openStatus = BCryptOpenAlgorithmProvider(
                &hashAlgorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                BCRYPT_ALG_HANDLE_HMAC_FLAG);
            if (!BCRYPT_SUCCESS(openStatus))
            {
                return false;
            }

            const NTSTATUS deriveStatus = BCryptDeriveKeyPBKDF2(
                hashAlgorithm,
                reinterpret_cast<PUCHAR>(const_cast<char*>(pin.data())),
                static_cast<ULONG>(pin.size()),
                salt,
                static_cast<ULONG>(sizeof(salt)),
                Pbkdf2Iterations,
                output,
                static_cast<ULONG>(KdfOutputSize),
                0);
            BCryptCloseAlgorithmProvider(hashAlgorithm, 0);
            SecureZeroMemory(salt, sizeof(salt));
            return BCRYPT_SUCCESS(deriveStatus);
        }

        bool HmacSha256(
            const unsigned char key[32],
            const unsigned char* message,
            std::size_t messageSize,
            unsigned char digest[ProofSize])
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            DWORD objectSize = 0;
            DWORD bytesCopied = 0;
            std::vector<unsigned char> hashObject;

            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            {
                return false;
            }

            NTSTATUS status = BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize),
                sizeof(objectSize),
                &bytesCopied,
                0);
            if (BCRYPT_SUCCESS(status))
            {
                hashObject.resize(objectSize);
                status = BCryptCreateHash(
                    algorithm,
                    &hash,
                    hashObject.data(),
                    objectSize,
                    const_cast<PUCHAR>(key),
                    32,
                    0);
            }
            if (BCRYPT_SUCCESS(status) && messageSize <= ULONG_MAX)
            {
                status = BCryptHashData(
                    hash,
                    const_cast<PUCHAR>(message),
                    static_cast<ULONG>(messageSize),
                    0);
            }
            else if (BCRYPT_SUCCESS(status))
            {
                status = STATUS_INVALID_PARAMETER;
            }
            if (BCRYPT_SUCCESS(status))
            {
                status = BCryptFinishHash(hash, digest, static_cast<ULONG>(ProofSize), 0);
            }

            if (hash)
            {
                BCryptDestroyHash(hash);
            }
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!hashObject.empty())
            {
                SecureZeroMemory(hashObject.data(), hashObject.size());
            }
            return BCRYPT_SUCCESS(status);
        }

        bool MakeProof(
            const unsigned char authKey[32],
            const char* label,
            const ClientHello& clientHello,
            const ServerHello& serverHello,
            unsigned char proof[ProofSize])
        {
            const std::size_t labelSize = std::strlen(label);
            std::vector<unsigned char> transcript(
                labelSize + sizeof(clientHello) + sizeof(serverHello));
            std::memcpy(transcript.data(), label, labelSize);
            std::memcpy(transcript.data() + labelSize, &clientHello, sizeof(clientHello));
            std::memcpy(
                transcript.data() + labelSize + sizeof(clientHello),
                &serverHello,
                sizeof(serverHello));
            const bool result = HmacSha256(
                authKey,
                transcript.data(),
                transcript.size(),
                proof);
            SecureZeroMemory(transcript.data(), transcript.size());
            return result;
        }

        bool ProofsMatch(
            const unsigned char expected[ProofSize],
            const unsigned char received[ProofSize]) noexcept
        {
            unsigned char difference = 0;
            for (std::size_t index = 0; index < ProofSize; ++index)
            {
                difference |= expected[index] ^ received[index];
            }
            return difference == 0;
        }

        bool DeriveAndAssign(
            const std::string& pin,
            const ClientHello& clientHello,
            const ServerHello& serverHello,
            SessionKeys& keys,
            unsigned char output[KdfOutputSize])
        {
            if (!DeriveKeys(pin, clientHello, serverHello, output))
            {
                return false;
            }
            std::copy(output + 32, output + 64, keys.serverToClient.begin());
            std::copy(output + 64, output + 96, keys.clientToServer.begin());
            return true;
        }
    }

    bool IsValidPin(const std::string& pin) noexcept
    {
        if (pin.size() != PinLength)
        {
            return false;
        }
        return std::all_of(pin.begin(), pin.end(), [](unsigned char value)
        {
            return value >= '0' && value <= '9';
        });
    }

    bool ClientHandshake(SOCKET socket, const std::string& pin, SessionKeys& keys)
    {
        if (!IsValidPin(pin))
        {
            return false;
        }

        ClientHello clientHello{};
        std::memcpy(clientHello.magic, ProtocolMagic, sizeof(ProtocolMagic));
        if (!RandomBytes(clientHello.nonce, sizeof(clientHello.nonce)) ||
            !SendAll(socket, reinterpret_cast<const unsigned char*>(&clientHello), sizeof(clientHello)))
        {
            return false;
        }

        ServerHello serverHello{};
        if (!ReceiveAll(socket, reinterpret_cast<unsigned char*>(&serverHello), sizeof(serverHello)) ||
            std::memcmp(serverHello.magic, ProtocolMagic, sizeof(ProtocolMagic)) != 0)
        {
            return false;
        }

        unsigned char keyMaterial[KdfOutputSize]{};
        if (!DeriveAndAssign(pin, clientHello, serverHello, keys, keyMaterial))
        {
            return false;
        }

        unsigned char clientProof[ProofSize]{};
        unsigned char expectedServerProof[ProofSize]{};
        unsigned char receivedServerProof[ProofSize]{};
        const bool proofCreated = MakeProof(
            keyMaterial,
            "Limeovery viewer authentication v2",
            clientHello,
            serverHello,
            clientProof);
        const bool serverProofCreated = MakeProof(
            keyMaterial,
            "Limeovery host authentication v2",
            clientHello,
            serverHello,
            expectedServerProof);
        SecureZeroMemory(keyMaterial, sizeof(keyMaterial));

        const bool result = proofCreated && serverProofCreated &&
            SendAll(socket, clientProof, sizeof(clientProof)) &&
            ReceiveAll(socket, receivedServerProof, sizeof(receivedServerProof)) &&
            ProofsMatch(expectedServerProof, receivedServerProof);
        SecureZeroMemory(clientProof, sizeof(clientProof));
        SecureZeroMemory(expectedServerProof, sizeof(expectedServerProof));
        SecureZeroMemory(receivedServerProof, sizeof(receivedServerProof));
        return result;
    }

    bool ServerHandshake(SOCKET socket, const std::string& pin, SessionKeys& keys)
    {
        if (!IsValidPin(pin))
        {
            return false;
        }

        ClientHello clientHello{};
        if (!ReceiveAll(socket, reinterpret_cast<unsigned char*>(&clientHello), sizeof(clientHello)) ||
            std::memcmp(clientHello.magic, ProtocolMagic, sizeof(ProtocolMagic)) != 0)
        {
            return false;
        }

        ServerHello serverHello{};
        std::memcpy(serverHello.magic, ProtocolMagic, sizeof(ProtocolMagic));
        if (!RandomBytes(serverHello.salt, sizeof(serverHello.salt)) ||
            !RandomBytes(serverHello.nonce, sizeof(serverHello.nonce)) ||
            !SendAll(socket, reinterpret_cast<const unsigned char*>(&serverHello), sizeof(serverHello)))
        {
            return false;
        }

        unsigned char keyMaterial[KdfOutputSize]{};
        if (!DeriveAndAssign(pin, clientHello, serverHello, keys, keyMaterial))
        {
            return false;
        }

        unsigned char expectedClientProof[ProofSize]{};
        unsigned char receivedClientProof[ProofSize]{};
        unsigned char serverProof[ProofSize]{};
        const bool clientProofCreated = MakeProof(
            keyMaterial,
            "Limeovery viewer authentication v2",
            clientHello,
            serverHello,
            expectedClientProof);
        const bool serverProofCreated = MakeProof(
            keyMaterial,
            "Limeovery host authentication v2",
            clientHello,
            serverHello,
            serverProof);
        SecureZeroMemory(keyMaterial, sizeof(keyMaterial));

        const bool result = clientProofCreated && serverProofCreated &&
            ReceiveAll(socket, receivedClientProof, sizeof(receivedClientProof)) &&
            ProofsMatch(expectedClientProof, receivedClientProof) &&
            SendAll(socket, serverProof, sizeof(serverProof));
        SecureZeroMemory(expectedClientProof, sizeof(expectedClientProof));
        SecureZeroMemory(receivedClientProof, sizeof(receivedClientProof));
        SecureZeroMemory(serverProof, sizeof(serverProof));
        return result;
    }

    AesGcmCipher::~AesGcmCipher()
    {
        Reset();
    }

    bool AesGcmCipher::Initialize(const std::array<unsigned char, 32>& key)
    {
        Reset();
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm,
            BCRYPT_AES_ALGORITHM,
            nullptr,
            0)))
        {
            Reset();
            return false;
        }

        if (!BCRYPT_SUCCESS(BCryptSetProperty(
            algorithm,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
            0)))
        {
            Reset();
            return false;
        }

        DWORD objectSize = 0;
        DWORD bytesCopied = 0;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &bytesCopied,
            0)))
        {
            Reset();
            return false;
        }

        keyObject.resize(objectSize);
        if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            algorithm,
            &keyHandle,
            keyObject.data(),
            objectSize,
            const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()),
            0)))
        {
            Reset();
            return false;
        }
        return true;
    }

    bool AesGcmCipher::Encrypt(
        std::uint64_t sequence,
        const unsigned char* additionalData,
        std::size_t additionalDataSize,
        const unsigned char* plaintext,
        std::size_t plaintextSize,
        std::vector<unsigned char>& ciphertextAndTag)
    {
        ciphertextAndTag.clear();
        if (!keyHandle || !plaintext || plaintextSize == 0 ||
            plaintextSize > ULONG_MAX || additionalDataSize > ULONG_MAX ||
            (!additionalData && additionalDataSize != 0) ||
            plaintextSize > std::numeric_limits<std::size_t>::max() - GcmTagLength)
        {
            return false;
        }

        unsigned char nonce[12]{};
        unsigned char tag[GcmTagLength]{};
        if (!MakeNonce(sequence, nonce))
        {
            return false;
        }

        ciphertextAndTag.resize(plaintextSize + GcmTagLength);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonce;
        authInfo.cbNonce = sizeof(nonce);
        authInfo.pbAuthData = const_cast<PUCHAR>(additionalData);
        authInfo.cbAuthData = static_cast<ULONG>(additionalDataSize);
        authInfo.pbTag = tag;
        authInfo.cbTag = sizeof(tag);

        ULONG encryptedSize = 0;
        const NTSTATUS status = BCryptEncrypt(
            keyHandle,
            const_cast<PUCHAR>(plaintext),
            static_cast<ULONG>(plaintextSize),
            &authInfo,
            nullptr,
            0,
            ciphertextAndTag.data(),
            static_cast<ULONG>(plaintextSize),
            &encryptedSize,
            0);
        if (!BCRYPT_SUCCESS(status) || encryptedSize != plaintextSize)
        {
            ciphertextAndTag.clear();
            return false;
        }
        std::memcpy(ciphertextAndTag.data() + plaintextSize, tag, sizeof(tag));
        SecureZeroMemory(tag, sizeof(tag));
        return true;
    }

    bool AesGcmCipher::Decrypt(
        std::uint64_t sequence,
        const unsigned char* additionalData,
        std::size_t additionalDataSize,
        const unsigned char* ciphertextAndTag,
        std::size_t ciphertextAndTagSize,
        std::vector<unsigned char>& plaintext)
    {
        plaintext.clear();
        if (!keyHandle || !ciphertextAndTag || ciphertextAndTagSize <= GcmTagLength ||
            ciphertextAndTagSize - GcmTagLength > ULONG_MAX ||
            additionalDataSize > ULONG_MAX || (!additionalData && additionalDataSize != 0))
        {
            return false;
        }

        const std::size_t ciphertextSize = ciphertextAndTagSize - GcmTagLength;
        unsigned char nonce[12]{};
        if (!MakeNonce(sequence, nonce))
        {
            return false;
        }

        unsigned char tag[GcmTagLength]{};
        std::memcpy(tag, ciphertextAndTag + ciphertextSize, sizeof(tag));
        plaintext.resize(ciphertextSize);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
        BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
        authInfo.pbNonce = nonce;
        authInfo.cbNonce = sizeof(nonce);
        authInfo.pbAuthData = const_cast<PUCHAR>(additionalData);
        authInfo.cbAuthData = static_cast<ULONG>(additionalDataSize);
        authInfo.pbTag = tag;
        authInfo.cbTag = sizeof(tag);

        ULONG decryptedSize = 0;
        const NTSTATUS status = BCryptDecrypt(
            keyHandle,
            const_cast<PUCHAR>(ciphertextAndTag),
            static_cast<ULONG>(ciphertextSize),
            &authInfo,
            nullptr,
            0,
            plaintext.data(),
            static_cast<ULONG>(ciphertextSize),
            &decryptedSize,
            0);
        SecureZeroMemory(tag, sizeof(tag));
        if (!BCRYPT_SUCCESS(status) || decryptedSize != ciphertextSize)
        {
            if (!plaintext.empty())
            {
                SecureZeroMemory(plaintext.data(), plaintext.size());
            }
            plaintext.clear();
            return false;
        }
        return true;
    }

    void AesGcmCipher::Reset() noexcept
    {
        if (keyHandle)
        {
            BCryptDestroyKey(keyHandle);
            keyHandle = nullptr;
        }
        if (algorithm)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
        }
        if (!keyObject.empty())
        {
            SecureZeroMemory(keyObject.data(), keyObject.size());
            keyObject.clear();
        }
    }

    bool AesGcmCipher::MakeNonce(std::uint64_t sequence, unsigned char nonce[12]) const noexcept
    {
        if (sequence == 0)
        {
            return false;
        }
        // Direction-specific keys make the counter nonce unique for the session.
        nonce[0] = 'L';
        nonce[1] = 'M';
        nonce[2] = 'V';
        nonce[3] = '2';
        for (int index = 0; index < 8; ++index)
        {
            nonce[4 + index] = static_cast<unsigned char>(sequence >> (56 - index * 8));
        }
        return true;
    }
}
