#pragma once

#include <cstdint>
#include <cstddef>

// Encrypt/decrypt data in-place using Twofish-256 in OFB (Output Feedback) mode.
// OFB is a stream cipher mode: the same operation encrypts and decrypts.
void twofish_ofb(const uint8_t key[32], const uint8_t iv[16],
                 uint8_t* data, size_t len);
                 