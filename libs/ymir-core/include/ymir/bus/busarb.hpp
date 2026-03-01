#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace busarb {

inline constexpr std::uint32_t kApiVersionMajor = 1;
inline constexpr std::uint32_t kApiVersionMinor = 2;
inline constexpr std::uint32_t kApiVersionPatch = 0;

enum class BusMasterId : std::uint8_t {
    SH2_A = 0,
    SH2_B = 1,
    DMA = 2,
};

struct TimingCallbacks {
    // Returns service duration in caller-defined tick units for a granted access.
    // Determinism contract: identical inputs must produce identical outputs.
    // Return value of 0 is treated as 1 tick by the arbiter.
    std::uint32_t (*access_cycles)(void *ctx, std::uint32_t addr, bool is_write, std::uint8_t size_bytes) = nullptr;
    void *ctx = nullptr;
};

struct BusRequest {
    BusMasterId master_id = BusMasterId::SH2_A;
    std::uint32_t addr = 0;
    bool is_write = false;
    std::uint8_t size_bytes = 4;
    // Opaque monotonic caller-owned timebase. Repeated queries at the same tick are valid.
    std::uint64_t now_tick = 0;
};

struct BusWaitResult {
    // should_wait=false implies wait_cycles=0.
    bool should_wait = false;
    // Stall-only delay in caller tick units until a request may begin.
    // This value is a minimum delay and does not predict future contention.
    std::uint32_t wait_cycles = 0;
};

struct ArbiterConfig {
    std::uint32_t same_address_contention = 2;
    std::uint32_t tie_turnaround = 1;
    bool enable_stats = false;
    std::uint64_t stats_report_interval = 1'000'000;
};

class Arbiter {
public:
    explicit Arbiter(TimingCallbacks callbacks, ArbiterConfig config = {});

    // Non-mutating wait query.
    [[nodiscard]] BusWaitResult query_wait(const BusRequest &req) const;
    // Mutating grant commit. Does not require a prior query_wait call.
    // duplicate commit_grant calls intentionally model duplicate grants.
    // had_tie indicates this request won a same-tick equal-priority tie.
    void commit_grant(const BusRequest &req, std::uint64_t tick_start, bool had_tie = false);
    void record_cache_hit_bypass(BusMasterId master_id, std::uint32_t addr);

    [[nodiscard]] std::optional<std::size_t> pick_winner(const std::vector<BusRequest> &same_tick_requests) const;
    [[nodiscard]] std::uint64_t bus_free_tick() const;
    [[nodiscard]] std::uint64_t bus_free_tick(std::uint32_t addr) const;
    // Defensive recovery for timeline divergence after state rewinds or booking anomalies.
    // If bus_free_tick(addr) is far ahead of now_tick, rebase domain state to now_tick.
    // Returns true when a rebase was applied.
    bool rebase_if_far_ahead(std::uint32_t addr, std::uint64_t now_tick, std::uint64_t max_ahead_cycles);

private:
    static constexpr std::size_t kDomainCount = 5;

    struct DomainState {
        std::uint64_t bus_free_tick = 0;
        bool has_last_granted_addr = false;
        std::uint32_t last_granted_addr = 0;
        std::optional<BusMasterId> last_granted_master = std::nullopt;
    };

    struct DomainStats {
        std::uint64_t query_calls = 0;
        std::uint64_t waited_calls = 0;
        std::uint64_t wait_cycles_sum = 0;
        std::uint32_t wait_cycles_max = 0;
        std::uint64_t grant_calls = 0;
        std::uint64_t base_cycles_sum = 0;
        std::uint64_t cache_hit_bypass = 0;
    };

    struct StatsState {
        std::uint64_t total_query_calls = 0;
        std::uint64_t next_report_at = 0;
        std::array<DomainStats, kDomainCount> domains{};
    };

    [[nodiscard]] static std::size_t domain_index(std::uint32_t addr);
    [[nodiscard]] DomainState &domain_state(std::uint32_t addr);
    [[nodiscard]] const DomainState &domain_state(std::uint32_t addr) const;
    [[nodiscard]] static const char *master_name(BusMasterId id);
    [[nodiscard]] static const char *domain_name(std::size_t domain);
    void maybe_report_stats() const;
    [[nodiscard]] std::uint32_t service_cycles(const BusRequest &req) const;
    [[nodiscard]] static int priority(BusMasterId id);

    TimingCallbacks callbacks_{};
    ArbiterConfig config_{};
    std::array<DomainState, kDomainCount> domain_states_{};
    mutable StatsState stats_{};
    std::optional<BusMasterId> last_granted_cpu_ = std::nullopt;
};

} // namespace busarb
