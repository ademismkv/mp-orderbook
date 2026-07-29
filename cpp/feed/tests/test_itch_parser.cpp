#include "itch_binaryfile_reader.h"
#include "itch_messages.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// Real-data verification for the ITCH 5.0 parser — reads an actual,
// downloaded-from-Nasdaq sample (see devlog for provenance: a 20MB prefix
// of 12302019.NASDAQ_ITCH50.gz, real trading data from Nasdaq's own
// historical archive, not synthetic), parses every message in it, and
// checks:
//   1. Every message's declared BinaryFile length prefix matches this
//      message type's spec-declared length exactly (a mismatch would mean
//      either the framing or a field-offset table is wrong).
//   2. The very first message is a System Event with event code 'O'
//      (Start of Messages) — the known, hand-verified expected value for
//      the start of any real trading day's file.
//   3. Every message type byte encountered is one this parser recognizes
//      (an unrecognized type byte on real data would mean either a
//      spec-coverage gap or a framing desync).
//   4. Report a message-type histogram so the counts can be eyeballed for
//      plausibility (e.g. Add Order should dominate; System Event should
//      be rare).

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/itch50_sample_20191230.bin";

    itch::BinaryFileReader reader(path);

    uint64_t total = 0;
    uint64_t unrecognized = 0;
    uint64_t length_mismatches = 0;
    std::map<char, uint64_t> counts;
    bool checked_first = false;
    bool first_ok = false;

    while (auto raw = reader.next_raw()) {
        ++total;
        const auto& buf = *raw;
        const char type = buf.empty() ? '\0' : static_cast<char>(buf[0]);

        const size_t expected = itch::expected_length(type);
        if (expected == 0) {
            ++unrecognized;
        } else if (expected != buf.size()) {
            ++length_mismatches;
            std::printf("length mismatch: type='%c' expected=%zu got=%zu at record #%llu\n", type,
                        expected, buf.size(), (unsigned long long)total);
        }

        auto parsed = itch::parse_message(buf.data(), buf.size());
        if (parsed) {
            counts[type]++;
            if (!checked_first) {
                checked_first = true;
                if (auto* se = std::get_if<itch::SystemEvent>(&*parsed)) {
                    first_ok = (se->event_code == 'O');
                    std::printf("first message: SystemEvent event_code='%c' (expect 'O') -> %s\n",
                                se->event_code, first_ok ? "OK" : "MISMATCH");
                } else {
                    std::printf("first message: not a SystemEvent -> MISMATCH\n");
                }
            }
        } else if (expected != 0) {
            // expected_length() recognized the type but parse_message()
            // still declined — means buf.size() < expected (truncated).
        }
    }

    std::printf("\n--- itch_parser real-data verification: %s ---\n", path.c_str());
    std::printf("total records read:     %llu\n", (unsigned long long)total);
    std::printf("unrecognized type byte:  %llu\n", (unsigned long long)unrecognized);
    std::printf("length mismatches:       %llu\n", (unsigned long long)length_mismatches);
    std::printf("first message check:     %s\n", first_ok ? "OK" : "FAILED");
    std::printf("message type histogram:\n");
    for (const auto& [type, count] : counts) {
        std::printf("  '%c'  %10llu\n", type, (unsigned long long)count);
    }

    const bool pass = (total > 0) && (unrecognized == 0) && (length_mismatches == 0) && first_ok;
    std::printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
