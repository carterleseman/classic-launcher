// Twofish-256 OFB mode : thin wrapper around the Counterpane reference
// implementation (twofish_ref.c).  The reference code is public-domain and
// passes all official test vectors.

#include "twofish.h"
#include <cstring>
#include <algorithm>

// Pull in the Counterpane types and function prototypes with C linkage.
extern "C" {
#include "twofish_aes.h"
    // reKey() is declared in twofish_aes.h but only as a prototype at the
    // bottom; re-declare here for clarity.
    int reKey(keyInstance* key);
}

void twofish_ofb(const uint8_t key[32], const uint8_t iv[16],
                 uint8_t* data, size_t len)
{
    keyInstance   ki;
    cipherInstance ci;

    // Initialise the key instance with a dummy makeKey() call (NULL material
    // skips ASCII hex parsing) then overwrite key32[] with our raw bytes and
    // call reKey() to run the key schedule.
    makeKey(&ki, DIR_ENCRYPT, 256, NULL);
    for (int i = 0; i < 8; i++) {
        ki.key32[i] =  static_cast<DWORD>(key[4*i    ])
                    | (static_cast<DWORD>(key[4*i + 1]) <<  8)
                    | (static_cast<DWORD>(key[4*i + 2]) << 16)
                    | (static_cast<DWORD>(key[4*i + 3]) << 24);
    }
    reKey(&ki);

    // ECB cipher instance (OFB is built manually on top).
    cipherInit(&ci, MODE_ECB, NULL);

    // OFB: encrypt the feedback block repeatedly to produce a keystream.
    uint8_t feedback[16];
    memcpy(feedback, iv, 16);

    size_t pos = 0;
    while (pos < len) {
        uint8_t ks[16];
        blockEncrypt(&ci, &ki,
            reinterpret_cast<BYTE*>(feedback), 128,
            reinterpret_cast<BYTE*>(ks));
        memcpy(feedback, ks, 16);   // next feedback = this block's ciphertext

        size_t n = std::min(len - pos, static_cast<size_t>(16));
        for (size_t i = 0; i < n; i++)
            data[pos + i] ^= ks[i];
        pos += n;
    }
}
