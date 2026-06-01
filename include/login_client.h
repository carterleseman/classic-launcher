#pragma once

#include <cstdint>
#include <string>

struct LoginResult {
    std::string ck2;       // Decrypted server credential (CK2), used to launch the game.
    uint64_t    user_id;   // Numeric user ID returned by the server.
    std::string username;
};

// ---------------------------------------------------------------------------
//  Authenticate against the KingsIsle login server.
//
//  Protocol (TCP, port 12000):
//    1. Receive SESSION_OFFER  (control opcode 0)
//    2. Send    SESSION_ACCEPT (control opcode 5)
//    3. Send    MSG_USER_AUTHEN_V3 (svc=7, msg=27) with Rec1 credential
//    4. Receive MSG_USER_AUTHEN_RSP (svc=7, msg=14)
//    5. Decrypt server Rec1 to obtain CK2
//
//  On auth failure the exception message contains the server's Reason string.
// ---------------------------------------------------------------------------
// Returns the MachineID that will be sent in MSG_USER_AUTHEN_V3 when no
// override is supplied: the first active adapter's MAC treated as a
// big-endian 48-bit integer (upper 16 bits zero).  Returns 0 on failure.
uint64_t get_machine_id_from_mac();

LoginResult login_authenticate(const std::string& username,
                               const std::string& password,
                               const std::string& uuid = {},
                               const std::string& hwid = {},
                               const char*        host = "login.us.wizard101.com",
                               uint16_t           port = 12000);
                               