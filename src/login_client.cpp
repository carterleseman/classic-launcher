#include "login_client.h"
#include "logger.h"
#include "dml.h"
#include "crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Derive MachineID from the first active network adapter's MAC address.
//  The real client treats the 6-byte MAC as a big-endian 48-bit integer in
//  the low 48 bits of a 64-bit value (upper 16 bits = 0).  Writing this as
//  a little-endian GID is equivalent to reversing the MAC bytes on the wire
// ---------------------------------------------------------------------------
uint64_t get_machine_id_from_mac()
{
    ULONG buf_size = 15 * 1024;
    std::vector<uint8_t> buf(buf_size);
    IP_ADAPTER_INFO* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());

    if (GetAdaptersInfo(adapters, &buf_size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buf_size);
        adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    }

    if (GetAdaptersInfo(adapters, &buf_size) != NO_ERROR)
        return 0;

    for (IP_ADAPTER_INFO* a = adapters; a != nullptr; a = a->Next) {
        if (a->AddressLength != 6)
            continue;
        uint64_t mid = 0;
        for (int i = 0; i < 6; ++i)
            mid |= static_cast<uint64_t>(a->Address[i]) << ((5 - i) * 8);
        return mid;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  Receive the next non-keep-alive packet.  Discards control keep-alive
//  frames (opcode 3) and returns keep-alive responses (opcode 4) unhandled
//  since the server should not require a response mid-auth.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> recv_skip_keepalive(SOCKET s) {
    for (;;) {
        auto raw = dml_recv(s);
        if (raw.size() >= 5 && raw[4] == 1 && raw[5] == DML_CTRL_KEEP_ALIVE) {
            continue; // Silently discard keep-alive probes from the server.
        }
        return raw;
    }
}

// ---------------------------------------------------------------------------

LoginResult login_authenticate(const std::string& username,
                               const std::string& password,
                               const std::string& uuid,
                               const std::string& hwid,
                               const char*        host,
                               uint16_t           port)
{
    LOG_STAT("LOGIN_CLIENT", "Connecting to " + std::string(host) + ":" + std::to_string(port));
    SOCKET s = dml_connect(host, port);
    LOG_STAT("LOGIN_CLIENT", "TCP connected : waiting for SESSION_OFFER");

    try {
        // ------------------------------------------------------------------
        // 1. Receive SESSION_OFFER (control opcode 0)
        // ------------------------------------------------------------------
        auto offer_raw = dml_recv(s);
        LOG_STAT("LOGIN_CLIENT", "Got SESSION_OFFER : sending SESSION_ACCEPT");
        SessionOffer offer = parse_session_offer(offer_raw);

        // ------------------------------------------------------------------
        // 2. Send SESSION_ACCEPT (control opcode 5)
        //    Uses the offer's SID and the client's current local time.
        // ------------------------------------------------------------------
        auto accept_pkt = build_session_accept(offer);
        dml_send(s, accept_pkt);
        LOG_STAT("LOGIN_CLIENT", "SESSION_ACCEPT sent : building Rec1");

        // ------------------------------------------------------------------
        // 3. Build Rec1 using the SESSION_OFFER's server timestamp.
        //    (key derivation depends on the server's sid / time_low / time_milli)
        // ------------------------------------------------------------------
        std::vector<uint8_t> rec1 = crypto_gen_rec1(
            username, password,
            offer.sid, offer.time_low, offer.time_milli);

        // ------------------------------------------------------------------
        // 4. Build and send MSG_USER_AUTHEN_V3 (svc=7, msg=27)
        //
        //    Field layout (from LoginMessages.xml, MsgOrder 27):
        //      Rec1          STR  : encrypted credential bytes
        //      Version       STR  : patch client version (empty)
        //      Revision      STR  : (empty)
        //      DataRevision  STR  : (empty)
        //      CRC           STR  : (empty)
        //      MachineID     GID  : reverse MAC
        //      Locale        STR  : "en-US"
        //      PatchClientID STR  : "{KI-UUID}:{HWID}"
        //        KI-UUID: HKCU\Software\KingsIsle\UUID
        //        HWID:    MD5 of sorted MOBO:/RAM: SMBIOS strings (see config.cpp)
        //      IsSteamPatcher UINT : 0
        //      ConsoleType   UBYT  : 0
        //      PlatformChatID STR : (empty)
        //      SteamID       STR  : 0
        //      SteamAuthTicket STR : (empty)
        // ------------------------------------------------------------------
        std::string patch_client_id = uuid + ":{" + hwid + "}";
        int64_t mid = static_cast<int64_t>(get_machine_id_from_mac());

        DmlWriter w;
        w.write_bytes(rec1);                     // Rec1
        w.write_str("");                         // Version
        w.write_str("");                         // Revision
        w.write_str("");                         // DataRevision
        w.write_str("");                         // CRC
        w.write_gid(mid);                        // MachineID
        w.write_str("en-US");                    // Locale
        w.write_str(patch_client_id);            // PatchClientID
        w.write_uint(0);                         // IsSteamPatcher
        w.write_ubyt(0);                         // ConsoleType
        w.write_str("");                         // PlatformChatID
        w.write_str("0");                        // SteamID
        w.write_str("");                         // SteamAuthTicket

        auto authen_pkt = build_dml_packet(SVC_LOGIN, MSG_USER_AUTHEN_V3, w.data());
        dml_send(s, authen_pkt);
        LOG_STAT("LOGIN_CLIENT", "MSG_USER_AUTHEN_V3 sent : waiting for response");

        // ------------------------------------------------------------------
        // 5. Receive MSG_USER_AUTHEN_RSP (svc=7, msg=14).
        //    Skip any keep-alive frames that arrive first.
        //
        //    Field layout (from LoginMessages.xml, MsgOrder 14):
        //      Error           INT  : 0 = success
        //      UserID          GID  : numeric user ID
        //      Rec1            STR  : server's encrypted credential (CK2 source)
        //      Reason          STR  : failure reason (non-empty on error)
        //      TimeStamp       STR
        //      PayingUser      INT
        //      Flags           INT
        //      SupportID       STR
        //      PublicPlayerName STR
        // ------------------------------------------------------------------
        auto rsp_raw = recv_skip_keepalive(s);
        LOG_STAT("LOGIN_CLIENT", "Got response packet : parsing");
        DmlPacket rsp = parse_dml_packet(rsp_raw);

        if (rsp.is_control)
            throw std::runtime_error("login_authenticate: expected DML response, got control packet");
        if (rsp.svc_id != SVC_LOGIN || rsp.msg_type != MSG_USER_AUTHEN_RSP)
            throw std::runtime_error("login_authenticate: unexpected message svc="
                                     + std::to_string(rsp.svc_id)
                                     + " msg=" + std::to_string(rsp.msg_type));

        DmlReader r(rsp.data);
        int32_t  error   = r.read_int();
        uint64_t user_id = static_cast<uint64_t>(r.read_gid());
        auto     srv_rec1 = r.read_bytes();   // server's Rec1 (encrypted CK2)
        std::string reason = r.read_str();

        LOG_STAT("LOGIN_CLIENT", "RSP: error=" + std::to_string(error)
                               + " user_id=" + std::to_string(user_id)
                               + " rec1_len=" + std::to_string(srv_rec1.size())
                               + " reason=\"" + reason + "\"");

        if (srv_rec1.empty())
            throw std::runtime_error(
                "login_authenticate: authentication failed"
                + (reason.empty() ? std::string{} : ": " + reason));

        // ------------------------------------------------------------------
        // 6. Decrypt the server's Rec1 to obtain CK2.
        // ------------------------------------------------------------------
        std::string ck2 = crypto_decrypt_rec1(srv_rec1,
                                              offer.sid,
                                              offer.time_low,
                                              offer.time_milli);

        dml_close(s);
        return LoginResult{ ck2, user_id, username };
    }
    catch (...) {
        dml_close(s);
        throw;
    }
}
