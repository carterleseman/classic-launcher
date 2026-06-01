#include "crypto.h"
#include "twofish.h"

#include <bcrypt.h>
#include <wincrypt.h>

#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>

static void dbg_hexdump_crypto(const char* label, const uint8_t* data, size_t len)
{
    std::ostringstream oss;
    oss << label << " (" << len << " bytes):";
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) oss << "\n  ";
        oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<unsigned>(data[i]) << ' ';
    }
    oss << '\n';
    OutputDebugStringA(oss.str().c_str());
}

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

// ---------------------------------------------------------------------------
//  SHA-512 via Windows BCrypt
// ---------------------------------------------------------------------------
std::vector<uint8_t> crypto_sha512(const void* data, size_t len) {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &alg, BCRYPT_SHA512_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("crypto_sha512: BCryptOpenAlgorithmProvider failed");

    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("crypto_sha512: BCryptCreateHash failed");
    }

    if (!BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(len), 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("crypto_sha512: BCryptHashData failed");
    }

    std::vector<uint8_t> result(64);
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, result.data(), 64, 0))) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("crypto_sha512: BCryptFinishHash failed");
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

// ---------------------------------------------------------------------------
//  Base64 encode via Windows Crypt32
// ---------------------------------------------------------------------------
std::string crypto_base64_encode(const void* data, size_t len) {
    DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;

    DWORD out_len = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data),
                         static_cast<DWORD>(len), flags, nullptr, &out_len);

    std::string result(out_len, '\0');
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data),
                         static_cast<DWORD>(len), flags, result.data(), &out_len);

    // CryptBinaryToStringA can include a null terminator in the count : trim it.
    while (!result.empty() && result.back() == '\0')
        result.pop_back();

    return result;
}

std::vector<uint8_t> crypto_base64_decode(const std::string& b64)
{
    DWORD out_len = 0;
    CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()),
                         CRYPT_STRING_BASE64, nullptr, &out_len, nullptr, nullptr);
    if (out_len == 0) return {};

    std::vector<uint8_t> result(out_len);
    CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()),
                         CRYPT_STRING_BASE64, result.data(), &out_len, nullptr, nullptr);
    result.resize(out_len);
    return result;
}

// ---------------------------------------------------------------------------
//  DPAPI password storage
// ---------------------------------------------------------------------------
std::string crypto_dpapi_encrypt(const std::string& plaintext)
{
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.c_str()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    std::string result = crypto_base64_encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::string crypto_dpapi_decrypt(const std::string& b64_ciphertext)
{
    std::vector<uint8_t> blob = crypto_base64_decode(b64_ciphertext);
    if (blob.empty()) return {};

    DATA_BLOB in{};
    in.pbData = blob.data();
    in.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return result;
}

// ---------------------------------------------------------------------------
//  Key / nonce derivation
// ---------------------------------------------------------------------------
void crypto_derive_key(uint16_t sid, uint32_t time_secs, uint32_t time_millis,
                       uint8_t out_key[32])
{
    // Start with the base pattern 0x17, 0x18, ..., 0x36.
    for (int i = 0; i < 32; i++)
        out_key[i] = static_cast<uint8_t>(0x17 + i);

    // Overwrite specific bytes with session-derived values.
    // sid: low byte at [4], zero at [5], high byte at [6]
    // key[4]=sid_bytes[0], key[5]=0, key[6]=sid_bytes[1])
    out_key[4]  = static_cast<uint8_t>(sid);
    out_key[5]  = 0;
    out_key[6]  = static_cast<uint8_t>(sid >> 8);

    // time_secs bytes (little-endian u32), scattered across the key:
    out_key[8]  = static_cast<uint8_t>(time_secs);        // s_bytes[0]
    out_key[9]  = static_cast<uint8_t>(time_secs >> 16);  // s_bytes[2]
    out_key[12] = static_cast<uint8_t>(time_secs >> 8);   // s_bytes[1]
    out_key[13] = static_cast<uint8_t>(time_secs >> 24);  // s_bytes[3]

    // time_millis bytes (little-endian u32):
    out_key[14] = static_cast<uint8_t>(time_millis);      // m_bytes[0]
    out_key[15] = static_cast<uint8_t>(time_millis >> 8); // m_bytes[1]
}

void crypto_derive_iv(uint8_t out_iv[16]) {
    // Fixed nonce: 0xB6, 0xB5, ..., 0xA7.
    for (int i = 0; i < 16; i++)
        out_iv[i] = static_cast<uint8_t>(0xB6 - i);
}

// ---------------------------------------------------------------------------
//  CK1 generation
// ---------------------------------------------------------------------------
std::string crypto_gen_ck1(const std::string& password,
                            uint16_t           sid,
                            uint32_t           time_secs,
                            uint32_t           time_millis)
{
    // H1 = SHA-512(password bytes)
    auto h1 = crypto_sha512(password.data(), password.size());

    // H1_b64 = base64(H1)  : 88-character standard base64 string
    std::string h1_b64 = crypto_base64_encode(h1.data(), h1.size());

    // concat = H1_b64 + decimal(sid) + decimal(time_secs) + decimal(time_millis)
    std::string concat = h1_b64
        + std::to_string(sid)
        + std::to_string(time_secs)
        + std::to_string(time_millis);

    // H2 = SHA-512(concat)
    auto h2 = crypto_sha512(concat.data(), concat.size());

    // CK1 = base64(H2)
    return crypto_base64_encode(h2.data(), h2.size());
}

// ---------------------------------------------------------------------------
//  Rec1 encrypt / decrypt
// ---------------------------------------------------------------------------
std::vector<uint8_t> crypto_gen_rec1(const std::string& username,
                                     const std::string& password,
                                     uint16_t           sid,
                                     uint32_t           time_secs,
                                     uint32_t           time_millis)
{
    std::string ck1 = crypto_gen_ck1(password, sid, time_secs, time_millis);

    // Plaintext record: "<sid_decimal> <username> <ck1>"
    std::string record = std::to_string(sid) + " " + username + " " + ck1;

    std::vector<uint8_t> bytes(record.begin(), record.end());

    OutputDebugStringA(("[crypto] Rec1 plaintext: \"" + record + "\"\n").c_str());

    uint8_t key[32], iv[16];
    crypto_derive_key(sid, time_secs, time_millis, key);
    crypto_derive_iv(iv);

    dbg_hexdump_crypto("[crypto] Twofish key", key, 32);
    dbg_hexdump_crypto("[crypto] Twofish IV ", iv,  16);

    twofish_ofb(key, iv, bytes.data(), bytes.size());
    return bytes;
}

std::string crypto_decrypt_rec1(std::vector<uint8_t> rec1_bytes,
                                 uint16_t             sid,
                                 uint32_t             time_secs,
                                 uint32_t             time_millis)
{
    uint8_t key[32], iv[16];
    crypto_derive_key(sid, time_secs, time_millis, key);
    crypto_derive_iv(iv);

    // OFB is symmetric : applying the keystream again decrypts.
    twofish_ofb(key, iv, rec1_bytes.data(), rec1_bytes.size());
    return std::string(rec1_bytes.begin(), rec1_bytes.end());
}
