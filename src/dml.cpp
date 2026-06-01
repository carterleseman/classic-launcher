#include "dml.h"

#include <cstring>
#include <cassert>

#pragma comment(lib, "ws2_32.lib")

// ---------------------------------------------------------------------------
//  Little-endian read/write helpers
// ---------------------------------------------------------------------------
static void push_le16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

static void push_le32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

static void push_le64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

static uint16_t load_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t load_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t load_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= (static_cast<uint64_t>(p[i]) << (i * 8));
    return v;
}

// ---------------------------------------------------------------------------
//  DmlWriter
// ---------------------------------------------------------------------------
void DmlWriter::write_ubyt(uint8_t v)   { buf_.push_back(v); }
void DmlWriter::write_byt(int8_t v)    { buf_.push_back(static_cast<uint8_t>(v)); }
void DmlWriter::write_ushrt(uint16_t v) { push_le16(buf_, v); }
void DmlWriter::write_shrt(int16_t v)  { push_le16(buf_, static_cast<uint16_t>(v)); }
void DmlWriter::write_uint(uint32_t v) { push_le32(buf_, v); }
void DmlWriter::write_int(int32_t v)   { push_le32(buf_, static_cast<uint32_t>(v)); }

void DmlWriter::write_flt(float v) {
    uint32_t tmp;
    std::memcpy(&tmp, &v, 4);
    push_le32(buf_, tmp);
}

void DmlWriter::write_gid(int64_t v) {
    push_le64(buf_, static_cast<uint64_t>(v));
}

void DmlWriter::write_str(const std::string& s) {
    push_le16(buf_, static_cast<uint16_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void DmlWriter::write_bytes(const std::vector<uint8_t>& bytes) {
    push_le16(buf_, static_cast<uint16_t>(bytes.size()));
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

// ---------------------------------------------------------------------------
//  DmlReader
// ---------------------------------------------------------------------------
static void check_bounds(size_t pos, size_t needed, size_t total) {
    if (pos + needed > total)
        throw std::runtime_error("DmlReader: read past end of buffer");
}

uint8_t DmlReader::read_ubyt() {
    check_bounds(pos_, 1, data_.size());
    return data_[pos_++];
}

int8_t DmlReader::read_byt() {
    return static_cast<int8_t>(read_ubyt());
}

uint16_t DmlReader::read_ushrt() {
    check_bounds(pos_, 2, data_.size());
    uint16_t v = load_le16(data_.data() + pos_);
    pos_ += 2;
    return v;
}

int16_t DmlReader::read_shrt() {
    return static_cast<int16_t>(read_ushrt());
}

uint32_t DmlReader::read_uint() {
    check_bounds(pos_, 4, data_.size());
    uint32_t v = load_le32(data_.data() + pos_);
    pos_ += 4;
    return v;
}

int32_t DmlReader::read_int() {
    return static_cast<int32_t>(read_uint());
}

float DmlReader::read_flt() {
    uint32_t tmp = read_uint();
    float v;
    std::memcpy(&v, &tmp, 4);
    return v;
}

int64_t DmlReader::read_gid() {
    check_bounds(pos_, 8, data_.size());
    int64_t v = static_cast<int64_t>(load_le64(data_.data() + pos_));
    pos_ += 8;
    return v;
}

std::string DmlReader::read_str() {
    uint16_t len = read_ushrt();
    check_bounds(pos_, len, data_.size());
    std::string s(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    return s;
}

std::vector<uint8_t> DmlReader::read_bytes() {
    uint16_t len = read_ushrt();
    check_bounds(pos_, len, data_.size());
    std::vector<uint8_t> v(data_.begin() + pos_, data_.begin() + pos_ + len);
    pos_ += len;
    return v;
}

// ---------------------------------------------------------------------------
//  Packet builders
// ---------------------------------------------------------------------------

// DML message packet layout:
//   [0-1]   magic   0xF00D (LE)
//   [2-3]   size    = total_bytes - 4 (LE)
//   [4]     is_control = 0
//   [5]     opcode     = 0
//   [6-7]   padding    = 0
//   [8]     svc_id
//   [9]     msg_type   (1-indexed MsgOrder)
//   [10-11] dml_len    = len(field_data) + 1 (null term) + 3
//   [12..]  field_data
//   [last]  null terminator byte
std::vector<uint8_t> build_dml_packet(
    uint8_t svc_id,
    uint8_t msg_type,
    const std::vector<uint8_t>& field_data)
{
    // data_len includes the trailing null terminator
    size_t   data_len   = field_data.size() + 1;
    // dml_len = svc_id(1) + msg_type(1) + dml_len(2) + field_data(N) = N + 4.
    // (does NOT include the trailing null terminator)
    uint16_t dml_len    = static_cast<uint16_t>(data_len + 3);
    uint16_t size_field = static_cast<uint16_t>(8 + data_len);

    std::vector<uint8_t> pkt;
    pkt.reserve(12 + data_len);
    push_le16(pkt, DML_MAGIC);
    push_le16(pkt, size_field);
    pkt.push_back(0);              // is_control
    pkt.push_back(0);              // opcode
    push_le16(pkt, 0);             // padding
    pkt.push_back(svc_id);
    pkt.push_back(msg_type);
    push_le16(pkt, dml_len);
    pkt.insert(pkt.end(), field_data.begin(), field_data.end());
    pkt.push_back(0);              // null terminator
    return pkt;
}

// Control packet layout:
//   [0-1]  magic   0xF00D (LE)
//   [2-3]  size    = 4 + len(data)
//   [4]    is_control = 1
//   [5]    opcode
//   [6-7]  reserved = 0
//   [8..]  data
std::vector<uint8_t> build_control_packet(uint8_t opcode, const std::vector<uint8_t>& data) {
    uint16_t size_field = static_cast<uint16_t>(4 + data.size());
    std::vector<uint8_t> pkt;
    pkt.reserve(8 + data.size());
    push_le16(pkt, DML_MAGIC);
    push_le16(pkt, size_field);
    pkt.push_back(1);              // is_control
    pkt.push_back(opcode);
    push_le16(pkt, 0);             // reserved
    pkt.insert(pkt.end(), data.begin(), data.end());
    return pkt;
}

// SESSION_ACCEPT (opcode 5) data fields:
//   reserved   u16  = 0
//   time_high  i32  = 0
//   time_low   i32  = client Unix seconds
//   time_milli u32  = client milliseconds
//   sid        u16  = from SESSION_OFFER
//   data_len   u32  = 1
//   data       u8   = 0
//   reserved   u8   = 0
std::vector<uint8_t> build_session_accept(const SessionOffer& offer) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // FILETIME is 100-ns intervals since 1601-01-01; convert to Unix epoch seconds
    uint32_t unix_secs = static_cast<uint32_t>(
        (uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
    uint32_t millis = st.wMilliseconds;

    std::vector<uint8_t> data;
    push_le16(data, 0);                  // reserved
    push_le32(data, 0);                  // time_high
    push_le32(data, unix_secs);          // time_low
    push_le32(data, millis);             // time_milli
    push_le16(data, offer.sid);          // sid
    push_le32(data, 1);                  // data_len
    data.push_back(0);                   // data
    data.push_back(0);                   // reserved
    return build_control_packet(DML_CTRL_SESSION_ACCEPT, data);
}

// ---------------------------------------------------------------------------
//  Packet parsers
// ---------------------------------------------------------------------------
DmlPacket parse_dml_packet(const std::vector<uint8_t>& raw) {
    if (raw.size() < 8)
        throw std::runtime_error("parse_dml_packet: packet too short");

    DmlPacket p{};
    p.is_control = (raw[4] != 0);
    p.opcode     = raw[5];

    if (p.is_control) {
        p.svc_id   = 0;
        p.msg_type = 0;
        // Control data starts at byte 8
        if (raw.size() > 8)
            p.data.assign(raw.begin() + 8, raw.end());
    } else {
        if (raw.size() < 12)
            throw std::runtime_error("parse_dml_packet: DML packet too short for header");
        p.svc_id   = raw[8];
        p.msg_type = raw[9];
        // Field data starts at byte 12; include everything (null terminator is
        // harmless : DmlReader stops after consuming the declared fields)
        if (raw.size() > 12)
            p.data.assign(raw.begin() + 12, raw.end());
    }
    return p;
}

// SESSION_OFFER wire layout:
//   byte 0-1:  magic
//   byte 2-3:  size
//   byte 4:    is_control = 1
//   byte 5:    opcode     = 0
//   byte 6-7:  padding    = 0
//   byte 8-9:  sid        u16
//   byte 10-13: time_high u32
//   byte 14-17: time_low  u32
//   byte 18-21: time_milli u32
//   byte 22-25: data_length u32
//   byte 26..(26+data_length-1): data
//   last byte: null_term
SessionOffer parse_session_offer(const std::vector<uint8_t>& raw) {
    if (raw.size() < 27)
        throw std::runtime_error("parse_session_offer: packet too short");

    SessionOffer s{};
    s.sid        = load_le16(raw.data() + 8);
    s.time_high  = load_le32(raw.data() + 10);
    s.time_low   = load_le32(raw.data() + 14);
    s.time_milli = load_le32(raw.data() + 18);
    uint32_t data_len = load_le32(raw.data() + 22);

    if (raw.size() < static_cast<size_t>(26) + data_len)
        throw std::runtime_error("parse_session_offer: data payload truncated");

    s.data.assign(raw.begin() + 26, raw.begin() + 26 + data_len);
    return s;
}

// ---------------------------------------------------------------------------
//  TCP socket helpers
// ---------------------------------------------------------------------------
void dml_winsock_init() {
    static bool done = false;
    if (done) return;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("dml_winsock_init: WSAStartup failed");
    done = true;
}

SOCKET dml_connect(const char* host, uint16_t port) {
    dml_winsock_init();

    addrinfo hints{}, *result = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[8];
    sprintf_s(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        throw std::runtime_error(std::string("dml_connect: getaddrinfo failed for ") + host);

    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        throw std::runtime_error("dml_connect: socket() failed");
    }

    if (connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
        closesocket(s);
        freeaddrinfo(result);
        throw std::runtime_error(std::string("dml_connect: connect() failed for ") + host);
    }

    freeaddrinfo(result);

    // 30-second receive timeout so a silent server never hangs us forever.
    DWORD timeout_ms = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    return s;
}

std::vector<uint8_t> dml_recv(SOCKET s) {
    // Peek at first 4 bytes: magic (2) + size (2).
    // Total packet length = size + 4.
    uint8_t hdr[4];
    int peeked = recv(s, reinterpret_cast<char*>(hdr), 4, MSG_PEEK);
    if (peeked < 4)
        throw std::runtime_error("dml_recv: failed to peek packet header");

    uint16_t size_field = load_le16(hdr + 2);
    int total = static_cast<int>(size_field) + 4;

    std::vector<uint8_t> buf(total);
    int received = 0;
    while (received < total) {
        int n = recv(s, reinterpret_cast<char*>(buf.data()) + received,
                     total - received, 0);
        if (n <= 0)
            throw std::runtime_error("dml_recv: connection closed or error");
        received += n;
    }
    return buf;
}

void dml_send(SOCKET s, const std::vector<uint8_t>& packet) {
    int total = static_cast<int>(packet.size());
    int sent  = 0;
    while (sent < total) {
        int n = send(s,
                     reinterpret_cast<const char*>(packet.data()) + sent,
                     total - sent, 0);
        if (n == SOCKET_ERROR)
            throw std::runtime_error("dml_send: send() failed");
        sent += n;
    }
}

void dml_close(SOCKET s) {
    if (s != INVALID_SOCKET)
        closesocket(s);
}
