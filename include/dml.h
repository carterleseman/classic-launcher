#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

// ---------------------------------------------------------------------------
//  Wire-level constants
// ---------------------------------------------------------------------------
static constexpr uint16_t DML_MAGIC              = 0xF00Du;
static constexpr uint8_t  DML_CTRL_SESSION_OFFER  = 0;
static constexpr uint8_t  DML_CTRL_UDP_HELLO      = 1;
static constexpr uint8_t  DML_CTRL_KEEP_ALIVE     = 3;
static constexpr uint8_t  DML_CTRL_KEEP_ALIVE_RSP = 4;
static constexpr uint8_t  DML_CTRL_SESSION_ACCEPT = 5;

// Service IDs
static constexpr uint8_t SVC_SYSTEM       = 1;
static constexpr uint8_t SVC_EXTENDEDBASE = 2;
static constexpr uint8_t SVC_LOGIN        = 7;
static constexpr uint8_t SVC_PATCH        = 8;

// Login message types (1-indexed MsgOrder from LoginMessages.xml)
static constexpr uint8_t MSG_USER_AUTHEN_RSP  = 14;
static constexpr uint8_t MSG_USER_VALIDATE    = 15;
static constexpr uint8_t MSG_USER_ADMIT_IND   = 20;
static constexpr uint8_t MSG_USER_AUTHEN_V3   = 27;

// Patch message types (1-indexed MsgOrder from PatchMessages.xml)
static constexpr uint8_t MSG_LATEST_FILE_LIST_V2 = 2;

// ---------------------------------------------------------------------------
//  Parsed generic packet
// ---------------------------------------------------------------------------
struct DmlPacket {
    bool     is_control;
    uint8_t  opcode;    // meaningful when is_control == true
    uint8_t  svc_id;    // meaningful when is_control == false
    uint8_t  msg_type;  // meaningful when is_control == false (1-indexed MsgOrder)
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
//  Session offer : control opcode 0, server -> client
//
//  Wire layout (data starting at byte 8 of the packet):
//    sid         u16
//    time_high   u32   (always 0 in practice)
//    time_low    u32   (Unix timestamp, seconds)
//    time_milli  u32   (milliseconds within the current second)
//    data_length u32
//    data[data_length]
//    null_term   u8
// ---------------------------------------------------------------------------
struct SessionOffer {
    uint16_t sid;
    uint32_t time_high;
    uint32_t time_low;
    uint32_t time_milli;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
//  DML field serializer
//  Builds the field portion of a DML message (not including the packet header).
// ---------------------------------------------------------------------------
class DmlWriter {
public:
    void write_ubyt(uint8_t v);
    void write_byt(int8_t v);
    void write_ushrt(uint16_t v);
    void write_shrt(int16_t v);
    void write_uint(uint32_t v);
    void write_int(int32_t v);
    void write_flt(float v);
    void write_gid(int64_t v);
    void write_str(const std::string& s);
    void write_bytes(const std::vector<uint8_t>& bytes); // STR field with raw byte content

    const std::vector<uint8_t>& data() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
//  DML field deserializer
//  Reads fields sequentially from the data portion of a parsed DmlPacket.
// ---------------------------------------------------------------------------
class DmlReader {
public:
    explicit DmlReader(const std::vector<uint8_t>& data)
        : data_(data), pos_(0) {}

    uint8_t              read_ubyt();
    int8_t               read_byt();
    uint16_t             read_ushrt();
    int16_t              read_shrt();
    uint32_t             read_uint();
    int32_t              read_int();
    float                read_flt();
    int64_t              read_gid();
    std::string          read_str();
    std::vector<uint8_t> read_bytes(); // STR field as raw bytes

    bool at_end() const { return pos_ >= data_.size(); }

private:
    const std::vector<uint8_t>& data_;
    size_t pos_;
};

// ---------------------------------------------------------------------------
//  Packet builders
// ---------------------------------------------------------------------------

// Wrap serialized DML field data into a complete on-wire DML packet.
std::vector<uint8_t> build_dml_packet(
    uint8_t svc_id,
    uint8_t msg_type,
    const std::vector<uint8_t>& field_data);

// Wrap raw data into a control packet.
std::vector<uint8_t> build_control_packet(
    uint8_t opcode,
    const std::vector<uint8_t>& control_data);

// Build the SESSION_ACCEPT response for a given SESSION_OFFER.
std::vector<uint8_t> build_session_accept(const SessionOffer& offer);

// ---------------------------------------------------------------------------
//  Packet parsers
// ---------------------------------------------------------------------------

// Parse a raw buffer (from dml_recv) into a DmlPacket.
DmlPacket    parse_dml_packet(const std::vector<uint8_t>& raw);

// Parse a SESSION_OFFER control packet.
SessionOffer parse_session_offer(const std::vector<uint8_t>& raw);

// ---------------------------------------------------------------------------
//  TCP socket helpers
// ---------------------------------------------------------------------------

void dml_winsock_init();

SOCKET dml_connect(const char* host, uint16_t port);

// Read exactly one complete DML/control packet from the socket.
// Peeks at the size field, then reads the full frame.
std::vector<uint8_t> dml_recv(SOCKET s);

void dml_send(SOCKET s, const std::vector<uint8_t>& packet);

void dml_close(SOCKET s);
