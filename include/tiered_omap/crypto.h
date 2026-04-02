#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace tiered_omap {

constexpr int AES_KEY_LEN = 16;   // AES-128
constexpr int AES_IV_LEN  = 16;   // CTR nonce

using CryptoKey = std::vector<uint8_t>;

inline CryptoKey generate_aes_key() {
    CryptoKey key(AES_KEY_LEN);
    if (RAND_bytes(key.data(), AES_KEY_LEN) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return key;
}

// AES-128-CTR encrypt.  Returns IV (16 B) || ciphertext.
inline std::vector<uint8_t> aes_encrypt(const CryptoKey& key,
                                         const std::vector<uint8_t>& plaintext) {
    std::vector<uint8_t> iv(AES_IV_LEN);
    if (RAND_bytes(iv.data(), AES_IV_LEN) != 1)
        throw std::runtime_error("RAND_bytes failed");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr,
                           key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }

    std::vector<uint8_t> out(AES_IV_LEN + plaintext.size());
    std::memcpy(out.data(), iv.data(), AES_IV_LEN);

    int outlen = 0;
    if (EVP_EncryptUpdate(ctx, out.data() + AES_IV_LEN, &outlen,
                          plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }

    int final_len = 0;
    EVP_EncryptFinal_ex(ctx, out.data() + AES_IV_LEN + outlen, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(AES_IV_LEN + outlen + final_len);
    return out;
}

// AES-128-CTR decrypt.  Input = IV (16 B) || ciphertext.
inline std::vector<uint8_t> aes_decrypt(const CryptoKey& key,
                                         const std::vector<uint8_t>& blob) {
    if (blob.size() < static_cast<size_t>(AES_IV_LEN))
        throw std::runtime_error("aes_decrypt: blob too small");

    const uint8_t* iv = blob.data();
    const uint8_t* ct = blob.data() + AES_IV_LEN;
    int ct_len = static_cast<int>(blob.size()) - AES_IV_LEN;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr,
                           key.data(), iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }

    std::vector<uint8_t> out(ct_len);
    int outlen = 0;
    if (EVP_DecryptUpdate(ctx, out.data(), &outlen, ct, ct_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }

    int final_len = 0;
    EVP_DecryptFinal_ex(ctx, out.data() + outlen, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(outlen + final_len);
    return out;
}

}  // namespace tiered_omap
