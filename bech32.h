// Copyright (c) 2017 Pieter Wuille
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bech32 is a string encoding format used in newer address types.
// The output consists of a human-readable part (alphanumeric), a
// separator character (1), and a base32 data section, the last
// 6 characters of which are a checksum.
//
// For more information, see BIP 173.

#include <stdint.h>
#include <string>
#include <vector>

namespace bech32
{

struct data {
    uint8_t* d;
    size_t len;
};

struct HRPX {
    std::string hrp;
    data hrpx;
    HRPX(const std::string& hrp_in);
    ~HRPX() { free(hrpx.d); }
};

extern HRPX bc;

/** Encode a Bech32 string. Returns the empty string in case of failure. */
std::string Encode(uint8_t** P, const HRPX& hrp, const std::vector<uint8_t>& values, bool skip_checksum = false);

/** Decode a Bech32 string. Returns (hrp, data). Empty hrp means failure. */
std::pair<std::string, std::vector<uint8_t>> Decode(uint8_t** P, const std::string& str);

/** Encode a Bech32 string. Returns the empty string in case of failure. */
inline std::string Encode(const std::string& hrp, const std::vector<uint8_t>& values) {
    uint8_t P[256];
    return Encode((uint8_t**)&P, HRPX(hrp), values);
}

/** Decode a Bech32 string. Returns (hrp, data). Empty hrp means failure. */
inline std::pair<std::string, std::vector<uint8_t>> Decode(const std::string& str) {
    uint8_t P[128];
    return Decode((uint8_t**)&P, str);
}

} // namespace bech32
