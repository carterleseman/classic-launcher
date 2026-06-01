#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Low-level crypto primitives
// ---------------------------------------------------------------------------

// Compute SHA-512 of arbitrary bytes using Windows BCrypt.
std::vector<uint8_t> crypto_sha512(const void* data, size_t len);

// Encode bytes to standard base64 (with '=' padding, no line breaks).
std::string crypto_base64_encode(const void* data, size_t len);

// Decode a standard base64 string back to raw bytes.
std::vector<uint8_t> crypto_base64_decode(const std::string& b64);

// ---------------------------------------------------------------------------
//  DPAPI password storage : encrypt/decrypt using the current Windows user's
//  credentials.  The encrypted blob is opaque on any other machine or account.
//
//  crypto_dpapi_encrypt  : returns a base64 string suitable for JSON storage.
//                          Returns "" on failure.
//  crypto_dpapi_decrypt  : takes the base64 string and returns the plaintext.
//                          Returns "" if decryption fails (e.g. wrong machine).
// ---------------------------------------------------------------------------
std::string crypto_dpapi_encrypt(const std::string& plaintext);
std::string crypto_dpapi_decrypt(const std::string& b64_ciphertext);

// ---------------------------------------------------------------------------
//  Rec1 key / nonce derivation
// ---------------------------------------------------------------------------

// Fill a 32-byte Twofish key derived from the session parameters.
void crypto_derive_key(uint16_t sid, uint32_t time_secs, uint32_t time_millis,
                       uint8_t out_key[32]);

// Fill a 16-byte fixed OFB nonce (does not depend on session parameters).
void crypto_derive_iv(uint8_t out_iv[16]);

// ---------------------------------------------------------------------------
//  CK1 / Rec1 helpers
// ---------------------------------------------------------------------------

// Compute CK1 (the client credential key) from the user's password and
// session parameters.  The result is a base64 string.
//
// Algorithm:
//   H1     = SHA-512(password_utf8)
//   H1_b64 = base64(H1)
//   concat = H1_b64 + decimal(sid) + decimal(time_secs) + decimal(time_millis)
//   H2     = SHA-512(concat_utf8)
//   CK1    = base64(H2)
std::string crypto_gen_ck1(const std::string& password,
                            uint16_t           sid,
                            uint32_t           time_secs,
                            uint32_t           time_millis);

// Build and encrypt a Rec1 record for MSG_USER_AUTHEN_V3.
// Plaintext format: "<sid_decimal> <username> <ck1>"
// Returned bytes are the encrypted record to send as the Rec1 DML field.
std::vector<uint8_t> crypto_gen_rec1(const std::string& username,
                                     const std::string& password,
                                     uint16_t           sid,
                                     uint32_t           time_secs,
                                     uint32_t           time_millis);

// Pass the raw encrypted bytes received from the server; returns the
// decrypted UTF-8 plaintext string.
std::string crypto_decrypt_rec1(std::vector<uint8_t> rec1_bytes,
                                 uint16_t             sid,
                                 uint32_t             time_secs,
                                 uint32_t             time_millis);
                                 