#include "ymir/bus/busarb.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace busarb {

Arbiter::Arbiter(TimingCallbacks callbacks, ArbiterConfig config) : callbacks_(callbacks), config_(config) {
    assert(callbacks_.access_cycles != nullptr && "TimingCallbacks.access_cycles must be non-null");
    if (config_.enable_stats) {
        stats_.next_report_at = std::max<std::uint64_t>(1, config_.stats_report_interval);
    }
}

BusWaitResult Arbiter::query_wait(const BusRequest &req) const {
    const std::size_t domain = domain_index(req.addr);
    const DomainState &state = domain_state(req.addr);
    BusWaitResult result{};
    if (req.now_tick >= state.bus_free_tick) {
        result = BusWaitResult{false, 0U};
    } else {
        const std::uint64_t delta = state.bus_free_tick - req.now_tick;
        result = BusWaitResult{true, static_cast<std::uint32_t>(std::min<std::uint64_t>(delta, 0xFFFFFFFFULL))};
    }

    if (result.should_wait && result.wait_cycles > 50000U) {
        std::fprintf(stderr,
                     "[busarb] spike master=%s domain=%s addr=%08X now=%llu free=%llu wait=%u\n",
                     master_name(req.master_id), domain_name(domain), static_cast<unsigned int>(req.addr),
                     static_cast<unsigned long long>(req.now_tick), static_cast<unsigned long long>(state.bus_free_tick),
                     result.wait_cycles);
    }

    if (config_.enable_stats) {
        DomainStats &stats = stats_.domains[domain];
        ++stats.query_calls;
        ++stats_.total_query_calls;
        if (result.should_wait) {
            ++stats.waited_calls;
            stats.wait_cycles_sum += result.wait_cycles;
            stats.wait_cycles_max = std::max(stats.wait_cycles_max, result.wait_cycles);
        }
        maybe_report_stats();
    }

    return result;
}

void Arbiter::commit_grant(const BusRequest &req, std::uint64_t tick_start, bool had_tie) {
    const std::size_t domain = domain_index(req.addr);
    DomainState &state = domain_state(req.addr);
    const std::uint64_t actual_start = std::max(tick_start, state.bus_free_tick);
    const std::uint32_t base_cycles = service_cycles(req);
    std::uint64_t duration = base_cycles;
    if (state.has_last_granted_addr && req.addr == state.last_granted_addr && state.last_granted_master.has_value() &&
        *state.last_granted_master != req.master_id) {
        duration += config_.same_address_contention;
    }
    if (had_tie) {
        duration += config_.tie_turnaround;
    }
    state.bus_free_tick = actual_start + duration;
    state.has_last_granted_addr = true;
    state.last_granted_addr = req.addr;
    state.last_granted_master = req.master_id;
    if (req.master_id == BusMasterId::SH2_A || req.master_id == BusMasterId::SH2_B) {
        last_granted_cpu_ = req.master_id;
    }

    if (config_.enable_stats) {
        DomainStats &stats = stats_.domains[domain];
        ++stats.grant_calls;
        stats.base_cycles_sum += base_cycles;
    }
}

void Arbiter::record_cache_hit_bypass(BusMasterId /*master_id*/, std::uint32_t addr) {
    if (!config_.enable_stats) {
        return;
    }
    const std::size_t domain = domain_index(addr);
    ++stats_.domains[domain].cache_hit_bypass;
}

std::optional<std::size_t> Arbiter::pick_winner(const std::vector<BusRequest> &same_tick_requests) const {
    if (same_tick_requests.empty()) {
        return std::nullopt;
    }

    std::size_t best = 0U;
    for (std::size_t i = 1; i < same_tick_requests.size(); ++i) {
        const auto &cand = same_tick_requests[i];
        const auto &cur = same_tick_requests[best];

        const int cprio = priority(cand.master_id);
        const int bprio = priority(cur.master_id);
        if (cprio > bprio) {
            best = i;
            continue;
        }
        if (cprio < bprio) {
            continue;
        }

        if (cand.master_id != BusMasterId::DMA && cur.master_id != BusMasterId::DMA && cand.master_id != cur.master_id) {
            BusMasterId preferred = BusMasterId::SH2_A;
            if (last_granted_cpu_.has_value()) {
                preferred = (*last_granted_cpu_ == BusMasterId::SH2_A) ? BusMasterId::SH2_B : BusMasterId::SH2_A;
            }
            if (cand.master_id == preferred) {
                best = i;
            }
            continue;
        }

        if (static_cast<int>(cand.master_id) < static_cast<int>(cur.master_id)) {
            best = i;
            continue;
        }
        if (cand.master_id != cur.master_id) {
            continue;
        }

        if (cand.addr < cur.addr) {
            best = i;
            continue;
        }
        if (cand.addr > cur.addr) {
            continue;
        }

        if (cand.is_write != cur.is_write && cand.is_write) {
            best = i;
            continue;
        }

        if (cand.size_bytes < cur.size_bytes) {
            best = i;
        }
    }
    return best;
}

std::uint64_t Arbiter::bus_free_tick() const {
    std::uint64_t max_tick = 0;
    for (const DomainState &state : domain_states_) {
        max_tick = std::max(max_tick, state.bus_free_tick);
    }
    return max_tick;
}

std::uint64_t Arbiter::bus_free_tick(std::uint32_t addr) const {
    return domain_state(addr).bus_free_tick;
}

bool Arbiter::rebase_if_far_ahead(std::uint32_t addr, std::uint64_t now_tick, std::uint64_t max_ahead_cycles) {
    DomainState &state = domain_state(addr);
    if (state.bus_free_tick <= now_tick) {
        return false;
    }
    const std::uint64_t delta = state.bus_free_tick - now_tick;
    if (delta <= max_ahead_cycles) {
        return false;
    }

    std::fprintf(stderr,
                 "[busarb] recover domain=%s addr=%08X now=%llu free=%llu delta=%llu action=rebase\n",
                 domain_name(domain_index(addr)), static_cast<unsigned int>(addr),
                 static_cast<unsigned long long>(now_tick), static_cast<unsigned long long>(state.bus_free_tick),
                 static_cast<unsigned long long>(delta));

    state.bus_free_tick = now_tick;
    state.has_last_granted_addr = false;
    state.last_granted_addr = 0;
    state.last_granted_master.reset();
    return true;
}

std::uint32_t Arbiter::service_cycles(const BusRequest &req) const {
    const std::uint32_t cycles = callbacks_.access_cycles(callbacks_.ctx, req.addr, req.is_write, req.size_bytes);
    return std::max(1U, cycles);
}

std::size_t Arbiter::domain_index(std::uint32_t addr) {
    if (addr <= 0x00F'FFFF) {
        return 0; // BIOS ROM
    }
    if (addr >= 0x020'0000 && addr <= 0x02F'FFFF) {
        return 1; // WRAM-L
    }
    if (addr >= 0x200'0000 && addr <= 0x4FF'FFFF) {
        return 2; // A-Bus CS0/CS1
    }
    if (addr >= 0x5A0'0000 && addr <= 0x5FB'FFFF) {
        return 4; // B-Bus (SCSP/VDP1/VDP2)
    }
    if (addr >= 0x600'0000 && addr <= 0x7FF'FFFF) {
        return 3; // WRAM-H
    }
    return 4; // fallback/unmanaged (grouped with B-Bus for stats/domain state)
}

Arbiter::DomainState &Arbiter::domain_state(std::uint32_t addr) {
    return domain_states_[domain_index(addr)];
}

const Arbiter::DomainState &Arbiter::domain_state(std::uint32_t addr) const {
    return domain_states_[domain_index(addr)];
}

const char *Arbiter::master_name(BusMasterId id) {
    switch (id) {
    case BusMasterId::SH2_A: return "SH2-A";
    case BusMasterId::SH2_B: return "SH2-B";
    case BusMasterId::DMA: return "DMA";
    }
    return "UNKNOWN";
}

const char *Arbiter::domain_name(std::size_t domain) {
    switch (domain) {
    case 0: return "BIOS";
    case 1: return "WRAM-L";
    case 2: return "A-BUS";
    case 3: return "WRAM-H";
    default: return "B-BUS";
    }
}

void Arbiter::maybe_report_stats() const {
    if (!config_.enable_stats) {
        return;
    }
    if (stats_.total_query_calls < stats_.next_report_at) {
        return;
    }

    std::fprintf(stderr, "[busarb] stats total_query_calls=%llu report_interval=%llu\n",
                 static_cast<unsigned long long>(stats_.total_query_calls),
                 static_cast<unsigned long long>(std::max<std::uint64_t>(1, config_.stats_report_interval)));

    for (std::size_t i = 0; i < kDomainCount; ++i) {
        const DomainStats &stats = stats_.domains[i];
        const bool has_activity = stats.query_calls > 0 || stats.grant_calls > 0 || stats.cache_hit_bypass > 0;
        if (!has_activity) {
            continue;
        }

        const double wait_avg = stats.query_calls > 0 ? static_cast<double>(stats.wait_cycles_sum) / stats.query_calls : 0.0;
        const double base_avg = stats.grant_calls > 0 ? static_cast<double>(stats.base_cycles_sum) / stats.grant_calls : 0.0;

        std::fprintf(stderr,
                     "[busarb] domain=%s queries=%llu waited=%llu wait_sum=%llu wait_avg=%.4f wait_max=%u grants=%llu "
                     "base_sum=%llu base_avg=%.4f cache_hit_bypass=%llu\n",
                     domain_name(i), static_cast<unsigned long long>(stats.query_calls),
                     static_cast<unsigned long long>(stats.waited_calls),
                     static_cast<unsigned long long>(stats.wait_cycles_sum), wait_avg, stats.wait_cycles_max,
                     static_cast<unsigned long long>(stats.grant_calls),
                     static_cast<unsigned long long>(stats.base_cycles_sum), base_avg,
                     static_cast<unsigned long long>(stats.cache_hit_bypass));
    }
    std::fflush(stderr);

    const std::uint64_t interval = std::max<std::uint64_t>(1, config_.stats_report_interval);
    while (stats_.next_report_at <= stats_.total_query_calls) {
        stats_.next_report_at += interval;
    }
}

int Arbiter::priority(BusMasterId id) {
    switch (id) {
    case BusMasterId::DMA: return 2;
    case BusMasterId::SH2_A: return 1;
    case BusMasterId::SH2_B: return 1;
    }
    return 0;
}

} // namespace busarb
