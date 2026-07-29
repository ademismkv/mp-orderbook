#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace itch {

// Reads Nasdaq's "BinaryFile" historical-data framing: each message is
// preceded by a 2-byte big-endian length prefix giving the length of the
// message payload that follows (NOT counting the 2 prefix bytes). E.g. the
// real bytes `00 0c 53 00 00 00 00 0a 11 ea 0e 8c 43 4f` are a length of
// 12 followed by a 12-byte System Event message starting with 'S'.
//
// This is confirmed against real downloaded Nasdaq sample data (see
// data/itch50_sample_20191230.bin), not assumed from the spec alone — the
// first message decodes to System Event / event code 'O' (Start of
// Messages), which is exactly what a real trading day's first message
// should be.
//
// This framing is distinct from MoldUDP64, which wraps the live multicast
// feed (session header + sequence numbers + heartbeats — see
// itch_moldudp64.h). The historical files firms download for
// backtesting/replay use this simpler length-prefixed framing instead,
// per Nasdaq's own "Accessing NASDAQ Historical TotalView-ITCH Data"
// document: "firms should use the BinaryFile protocol to process the
// TotalView-ITCH 5.0 ... files."
class BinaryFileReader {
public:
    explicit BinaryFileReader(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
        if (!f_) throw std::runtime_error("BinaryFileReader: could not open " + path);
    }
    ~BinaryFileReader() {
        if (f_) std::fclose(f_);
    }
    BinaryFileReader(const BinaryFileReader&) = delete;
    BinaryFileReader& operator=(const BinaryFileReader&) = delete;

    // Reads the next length-prefixed record's raw bytes (starting at the
    // message type byte, length-prefix bytes not included). Returns
    // nullopt at clean EOF, or if fewer bytes remain than the length
    // prefix claims — expected at the tail of a deliberately-trimmed
    // sample file, and handled as "stop, don't crash," not an error.
    std::optional<std::vector<uint8_t>> next_raw() {
        uint8_t len_bytes[2];
        if (std::fread(len_bytes, 1, 2, f_) != 2) return std::nullopt;   // EOF
        const size_t len = (static_cast<size_t>(len_bytes[0]) << 8) | len_bytes[1];
        if (len == 0) return std::nullopt;

        std::vector<uint8_t> buf(len);
        const size_t got = std::fread(buf.data(), 1, len, f_);
        if (got != len) return std::nullopt;   // truncated trailing record — stop cleanly
        return buf;
    }

private:
    std::FILE* f_;
};

} // namespace itch
