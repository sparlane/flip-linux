#pragma once
#include <cstdint>
#include <array>
#include <arpa/inet.h>

// IEEE 802.3 Ethertype for FLIP
constexpr uint16_t FLIP_ETHERTYPE = 0x8146;

static inline uint16_t flip_ethertype_network(void)
{
    return htons(FLIP_ETHERTYPE);
}

// Fragment Control Header
struct fc_header {
    uint8_t fc_type;
    uint8_t fc_cnt;
} __attribute__((packed));

typedef uint64_t flip_address_t;

// FLIP Layer 3 Packet Header
struct flip_packet {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint8_t reserved;
    uint16_t actual_hopcount;
    uint16_t max_hopcount;
    flip_address_t dst_address;
    flip_address_t src_address;
    uint32_t message_id;
    uint32_t length;
    uint32_t offset;
    uint32_t total_length;
} __attribute__((packed));

// FLIP packet types
enum class flip_type : uint8_t {
    LOCATE      = 1,
    HEREIS      = 2,
    UNIDATA     = 3,
    MULTIDATA   = 4,
    NOTHERE     = 5,
    UNTRUSTED   = 6,
};

// FLIP flags
constexpr uint8_t FLIP_FLAG_ENDIAN        = 0x01;  // Little endian if set
constexpr uint8_t FLIP_FLAG_VARIABLE_PART = 0x02;  // Variable part follows header
constexpr uint8_t FLIP_FLAG_SECURITY      = 0x04;  // Trusted networks only
constexpr uint8_t FLIP_FLAG_UNREACHABLE   = 0x10;  // Unreachable via trusted networks
constexpr uint8_t FLIP_FLAG_UNSAFE        = 0x20;  // Went over untrusted network