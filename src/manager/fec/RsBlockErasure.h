#ifndef WINJECT_MANAGER_RS_BLOCK_ERASURE_H_
#define WINJECT_MANAGER_RS_BLOCK_ERASURE_H_

#include <chrono>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <vector>

namespace winject
{

// Packet-block Reed-Solomon erasure FEC (systematic Cauchy MDS via ISA-L).
// k original UDP datagrams become n on-air shards; any k of n recover the
// originals. Systematic shards are native size (2-byte length + payload);
// RS math and parity shards pad to the longest row. Not compatible with
// tools/fec.py (reedsolo) parity.
//
// Shards are marked by the LC header FEC flag, not by their content. Shard
// header (5 bytes, same layout as vstreamer's rs_block_erasure):
//   [0..1] sdu_base (BE u16): SDU sequence of data shard 0; the block id
//   [2..3] BE u16, MSB first: k (5) | n (5) | idx (5) | spare (1)
//   [4]    MSB first: sdu_n (5) | spare (3)
// idx >= k is parity. A block flushed short carries sdu_n < k datagrams; data
// slots sdu_n..k-1 are empty pads that are not sent.
class RsBlockErasure
{
public:
    static constexpr size_t k_header_len = 5;
    static constexpr size_t k_len_prefix = 2;
    static constexpr int k_default_timeout_ms = 20;
    // k and n are 5-bit fields on the wire.
    static constexpr size_t k_max_n = 31;
    static constexpr size_t k_done_max = 128;
    static constexpr size_t k_block_max = 64;

    // Incomplete RX block TTL, and finished-id TTL (duplicate-shard
    // suppression). done_hold is longer so a late shard of a completed block
    // is still dropped, but short enough that a peer restart which reuses
    // sdu_base from an earlier session is accepted after the old ids age out.
    int rx_hold_ms() const;
    int done_hold_ms() const;

    RsBlockErasure();

    // init() / disable() keep the TX SDU sequence running (random start), so a
    // runtime k/n change or restart does not replay ids the peer has seen.
    bool init(int k, int n, int timeout_ms);
    // Stop TX encode; flush pending first via flush()/announce_down. RX decode
    // still works from shard headers.
    void disable();
    bool enabled() const
    {
        return enabled_;
    }
    int k() const
    {
        return k_;
    }
    int n() const
    {
        return n_;
    }
    const char* impl_name() const;

    // App datagram -> zero or more air shards (full block or nothing).
    void push_app(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);
    void flush(std::vector<std::vector<uint8_t>>* out);
    void flush_if_deadline();
    bool has_pending() const;
    bool has_tx_shards() const;
    size_t first_tx_shard_size() const;
    bool pop_tx_shard(size_t max, std::vector<uint8_t>* shard);
    void drain_tx_shards(std::vector<std::vector<uint8_t>>* out);

    // Air shard (shard header + body) -> original payloads when its block
    // decodes. Only call it for slots whose LC header has the FEC flag.
    void push_air(const uint8_t* data, size_t len,
                  std::vector<std::vector<uint8_t>>* out);

    uint16_t tx_sdu_seq() const
    {
        return sdu_seq_;
    }
    // Overrides the random start; tests use it to cross the u16 wrap.
    void set_tx_sdu_seq(uint16_t seq)
    {
        sdu_seq_ = seq;
    }

    uint64_t recovered() const
    {
        return recovered_;
    }
    uint64_t blocks() const
    {
        return blocks_;
    }
    uint64_t decode_fail() const
    {
        return decode_fail_;
    }
    uint64_t oversized() const
    {
        return oversized_;
    }
    // Interval since last take. recovered() / decode_fail() stay lifetime.
    uint64_t take_recovered();
    uint64_t take_decode_fail();

    // Shards not yet pulled for TX plus app datagrams awaiting encode.
    void tx_pending_stats(uint64_t* pkt, uint64_t* byt) const;

    // Encode one block. 1 <= packets.size() <= k; data slots past
    // packets.size() are empty pads and are not emitted, so out holds
    // packets.size() data shards then n - k parity shards. Data shards omit
    // trailing zeros on the wire; parity is full width.
    bool encode_block(const std::vector<std::vector<uint8_t>>& packets,
                      uint16_t sdu_base,
                      std::vector<std::vector<uint8_t>>* out) const;
    // Decode from shard index -> body. k/n/sdu_n come from the wire header (or
    // from init() via the overload, with sdu_n = k). Data slots sdu_n..k-1
    // count as empty pads. Returns false if unrecoverable.
    bool decode_block(
        int k, int n, int sdu_n,
        const std::unordered_map<int, std::vector<uint8_t>>& frags,
        std::vector<std::vector<uint8_t>>* payloads, int* recovered) const;
    bool decode_block(
        const std::unordered_map<int, std::vector<uint8_t>>& frags,
        std::vector<std::vector<uint8_t>>* payloads, int* recovered) const;

    static size_t max_original();

    static bool pack_header(uint8_t* out, uint16_t sdu_base, int index, int k,
                            int n, int sdu_n);
    static bool unpack_header(const uint8_t* data, size_t len,
                              uint16_t* sdu_base, int* index, int* k, int* n,
                              int* sdu_n);

private:
    struct rx_block_s
    {
        int k = 0;
        int n = 0;
        int sdu_n = 0;
        std::unordered_map<int, std::vector<uint8_t>> frags;
        std::chrono::steady_clock::time_point first_seen{};
    };

    void expire_rx();
    void expire_done();
    void mark_done(uint16_t sdu_base);
    std::chrono::steady_clock::time_point now() const;
    static int gen_decode_matrix(int k, int n, const uint8_t* encode_matrix,
                                 const uint8_t* err_list, int nerrs,
                                 uint8_t* decode_matrix, uint8_t* decode_index);

    bool enabled_ = false;
    int k_ = 0;
    int n_ = 0;
    int p = 0;
    int timeout_ms = k_default_timeout_ms;
    std::vector<uint8_t> encode_matrix;
    std::vector<uint8_t> g_tbls;

    std::deque<std::vector<uint8_t>> tx_shards_;
    std::vector<std::vector<uint8_t>> pending;
    std::chrono::steady_clock::time_point deadline{};
    bool deadline_set = false;
    uint16_t sdu_seq_ = 0;

    std::unordered_map<uint16_t, rx_block_s> rx_blocks;
    std::deque<uint16_t> done_order;
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> done;

    uint64_t recovered_ = 0;
    uint64_t recovered_seen_ = 0;
    uint64_t blocks_ = 0;
    uint64_t decode_fail_ = 0;
    uint64_t decode_fail_seen_ = 0;
    uint64_t oversized_ = 0;
};

}  // namespace winject

#endif  // WINJECT_MANAGER_RS_BLOCK_ERASURE_H_
