#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/packed_directory.h"
#include "tiered_omap/workload.h"

#include "oram/pathoram/oram.hpp"
#include "otree/otree.hpp"
#include "external_memory/server/serverBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

using EnigPathOram =
    _ORAM::PathORAM::ORAMClient::ORAMClient<_OBST::Node, ORAM__Z, false, 4>;
using EnigOramClient = _OBST::OramClient::OramClient<EnigPathOram>;
using EnigObst = _OBST::OBST::OBST<EnigOramClient>;
using EnigStashedBlock = EnigObst::StashedBlock_t;
using EnigLargeBucket = EnigPathOram::ORAMClientInterface_t::LargeBucket_t;

struct Config {
    int min_logN = 8;
    int max_logN = 12;
    int n = 128;
    int value_size = 256;
    int Q = 50;
    double s = 1.0;
    std::string env = "both";      // large | constrained | both | hardware
    int trusted_kb = 8192;         // used by the constrained model
    double page_us = 8.0;          // synthetic EPC miss/page cost
    std::string outdir = "tee_results_original_enigmap";
};

static void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --min_logN N       first log2 database size\n"
        << "  --max_logN N       last log2 database size\n"
        << "  --n N              hot-set capacity\n"
        << "  --val BYTES        value size\n"
        << "  --Q N              queries per point\n"
        << "  --s X              Zipf skew\n"
        << "  --env MODE         large | constrained | both | hardware\n"
        << "  --trusted_kb KB    trusted-memory budget for constrained model\n"
        << "  --page_us US       synthetic page-miss penalty for constrained model\n"
        << "  --outdir DIR       output directory\n";
}

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--help" || k == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (i + 1 >= argc) {
            throw std::runtime_error("missing value for " + k);
        }
        std::string v = argv[++i];
        if (k == "--min_logN") c.min_logN = std::stoi(v);
        else if (k == "--max_logN") c.max_logN = std::stoi(v);
        else if (k == "--n") c.n = std::stoi(v);
        else if (k == "--val") c.value_size = std::stoi(v);
        else if (k == "--Q") c.Q = std::stoi(v);
        else if (k == "--s") c.s = std::stod(v);
        else if (k == "--env") c.env = v;
        else if (k == "--trusted_kb") c.trusted_kb = std::stoi(v);
        else if (k == "--page_us") c.page_us = std::stod(v);
        else if (k == "--outdir") c.outdir = v;
        else throw std::runtime_error("unknown option " + k);
    }
    return c;
}

static std::vector<std::pair<int, Bytes>> make_data(int N, int val_size) {
    std::vector<std::pair<int, Bytes>> data;
    data.reserve(N);
    for (int i = 0; i < N; ++i)
        data.push_back({i, pad_bytes(int_to_bytes(i), val_size)});
    return data;
}

static std::vector<int> make_hot_keys(int n) {
    std::vector<int> hk(n);
    for (int i = 0; i < n; ++i) hk[i] = i;
    return hk;
}

static void ensure_dir(const std::string& dir) {
    ::mkdir(dir.c_str(), 0755);
}

static double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

static void log_stage(int logN, const std::string& stage,
                      Clock::time_point start) {
    std::cout << "logN=" << logN << " stage=" << stage
              << " sec=" << std::fixed << std::setprecision(3)
              << seconds_since(start) << "\n";
}

static void verify_value(int key, const Bytes& value) {
    if (bytes_to_int(value) != key) {
        throw std::runtime_error(
            "wrong value for key " + std::to_string(key));
    }
}

struct MemoryProfile {
    std::string name;
    int trusted_kb = 0;
    double page_us = 0.0;
    bool model_paging = false;
};

struct Measurement {
    std::string config;
    bool flat = false;
    bool tier_membership = false;
    double hot_us = 0.0;
    double cold_us = 0.0;
    double total_us = 0.0;
    double answer_us = 0.0;
    double hit_pct = -1.0;
    int hot_count = 0;
    int cold_count = 0;
    double avg_hot_pages = 0.0;
};

static constexpr uint64_t PAGE_SIZE = EnclaveOram::PAGE_SIZE;

static uint64_t next_power_of_two(uint64_t n) {
    uint64_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static uint64_t ceil_log2_u64(uint64_t n) {
    uint64_t p = 1, l = 0;
    while (p < n) {
        p <<= 1;
        ++l;
    }
    return l;
}

static uint64_t estimate_enig_backend_bytes(int capacity) {
    using LargeBucket = EnigPathOram::ORAMClientInterface_t::LargeBucket_t;
    uint64_t logical_n = static_cast<uint64_t>(std::max(capacity, 4)) + 2;
    uint64_t levels = ceil_log2_u64(logical_n) + 1;
    uint64_t packed_levels =
        ((levels + ORAM_SERVER__LEVELS_PER_PACK - 1)
         / ORAM_SERVER__LEVELS_PER_PACK)
        * ORAM_SERVER__LEVELS_PER_PACK;
    uint64_t packed_bucket_domain = uint64_t{1} << packed_levels;
    uint64_t large_buckets =
        packed_bucket_domain / LargeBucket::BUCKETS_PER_PACK + 2;
    return large_buckets * sizeof(LargeBucket);
}

static void reset_enig_default_backend(uint64_t required_bytes) {
    constexpr uint64_t kSlackBytes = 256ULL << 20;
    uint64_t backend_bytes = required_bytes + kSlackBytes;
    delete EM::Backend::g_DefaultBackend;
    EM::Backend::g_DefaultBackend =
        new EM::Backend::MemServerBackend(backend_bytes);
}

static std::vector<MemoryProfile> make_profiles(const Config& cfg) {
    if (cfg.env == "large") {
        return {{"large", 0, 0.0, false}};
    }
    if (cfg.env == "constrained" || cfg.env == "small") {
        return {{"constrained", cfg.trusted_kb, cfg.page_us, true}};
    }
    if (cfg.env == "hardware") {
        return {{"hardware", cfg.trusted_kb, 0.0, false}};
    }
    if (cfg.env == "both") {
        return {
            {"large", 0, 0.0, false},
            {"constrained", cfg.trusted_kb, cfg.page_us, true},
        };
    }
    throw std::runtime_error("unknown --env " + cfg.env);
}

using EnigBlock = EnigPathOram::Block_t;
using EnigStashedBlock = _ORAM::StashedBlock::StashedBlock<EnigBlock>;

static uint64_t estimate_enigmap_working_bytes(int capacity) {
    uint64_t leaves = next_power_of_two(static_cast<uint64_t>(capacity) + 2);
    uint64_t buckets = 2 * leaves - 1;
    return buckets * ORAM__Z * sizeof(EnigStashedBlock);
}

static uint64_t estimate_packed_hot_working_bytes(int hot_capacity,
                                                  int value_size) {
    uint64_t cap = static_cast<uint64_t>(std::max(hot_capacity, 1));
    uint64_t leaves = next_power_of_two(cap);
    uint64_t buckets = 2 * leaves - 1;
    uint64_t block_bytes = 2 * sizeof(int) + static_cast<uint64_t>(value_size);
    uint64_t oram_bytes = buckets * 4 * block_bytes;
    uint64_t dir_bytes = cap * 20;
    return oram_bytes + dir_bytes;
}

static double estimate_enigmap_query_pages(int capacity) {
    uint64_t logical_n = static_cast<uint64_t>(capacity) + 2;
    uint64_t max_depth = ceil_log2_u64(next_power_of_two(logical_n));
    max_depth += (max_depth >> 1) + 1;
    uint64_t path_buckets = ceil_log2_u64(next_power_of_two(logical_n)) + 1;
    uint64_t bucket_bytes = ORAM__Z * sizeof(EnigStashedBlock);
    uint64_t bucket_pages = std::max<uint64_t>(
        1, (bucket_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    return static_cast<double>(max_depth * path_buckets * bucket_pages * 2);
}

static double apply_memory_model(double measured_us,
                                 const MemoryProfile& profile,
                                 uint64_t working_bytes,
                                 double pages_per_query) {
    if (!profile.model_paging || profile.trusted_kb <= 0 || profile.page_us <= 0.0)
        return measured_us;

    double trusted_bytes = static_cast<double>(profile.trusted_kb) * 1024.0;
    if (static_cast<double>(working_bytes) <= trusted_bytes)
        return measured_us;

    double spill_ratio =
        (static_cast<double>(working_bytes) - trusted_bytes)
        / static_cast<double>(working_bytes);
    return measured_us + spill_ratio * pages_per_query * profile.page_us;
}

static double modeled_total(const Measurement& m,
                            const MemoryProfile& profile,
                            int N,
                            int n,
                            int value_size) {
    uint64_t cold_working = estimate_enigmap_working_bytes(N);
    double cold_pages = estimate_enigmap_query_pages(N);
    uint64_t hot_working = estimate_packed_hot_working_bytes(n, value_size);

    if (m.flat) {
        return apply_memory_model(m.total_us, profile, cold_working, cold_pages);
    }

    if (!m.tier_membership) {
        uint64_t combined_working = cold_working + hot_working;
        double combined_pages = cold_pages + m.avg_hot_pages;
        return apply_memory_model(
            m.total_us, profile, combined_working, combined_pages);
    }

    double hot_model = apply_memory_model(
        m.hot_us, profile, hot_working, m.avg_hot_pages);
    double cold_model = apply_memory_model(
        m.cold_us, profile, cold_working, cold_pages);

    double total = 0.0;
    if (m.hot_count > 0) total += hot_model * m.hot_count;
    if (m.cold_count > 0) total += cold_model * m.cold_count;
    int q = m.hot_count + m.cold_count;
    return q > 0 ? total / q : 0.0;
}

static uint64_t heap_node_for_path_bucket(uint64_t leaf,
                                          uint64_t depth,
                                          uint64_t height) {
    return (uint64_t{1} << depth) - 1 + (leaf >> (height - depth));
}

static std::pair<uint64_t, _ORAM::ORAMAddress> build_balanced_obst_nodes(
    std::vector<EnigStashedBlock>& nodes,
    uint64_t l,
    uint64_t r,
    const _ORAM::ORAMAddress& fake_node) {
    uint64_t m = l + (r - l) / 2;
    uint64_t left_h = 0;
    uint64_t right_h = 0;
    _ORAM::ORAMAddress children[2] = {fake_node, fake_node};

    if (m > l) {
        auto left = build_balanced_obst_nodes(nodes, l, m - 1, fake_node);
        left_h = left.first;
        children[0] = left.second;
    }
    if (r > m) {
        auto right = build_balanced_obst_nodes(nodes, m + 1, r, fake_node);
        right_h = right.first;
        children[1] = right.second;
    }

    auto& node = nodes[m];
    node.block.data.child[0] = children[0];
    node.block.data.child[1] = children[1];
    node.block.data.balance = _OBST::B_BALANCED;
    if (right_h > left_h) {
        node.block.data.balance = _OBST::B_RIGHT;
    } else if (left_h > right_h) {
        node.block.data.balance = _OBST::B_LEFT;
    }
    return {1 + std::max(left_h, right_h), node.oaddress};
}

static void place_blocks_in_enig_path_oram(
    EnigObst& map,
    const std::vector<EnigStashedBlock>& blocks) {
    auto& path_oram = map.oram.oram;
    const uint64_t height = path_oram.L_;
    const uint64_t bucket_count = (uint64_t{1} << (height + 1)) - 1;
    std::vector<uint8_t> occupancy(bucket_count, 0);
    std::vector<EnigStashedBlock> overflow;
    std::unordered_map<uint64_t, EnigLargeBucket> large_buckets;
    large_buckets.reserve(blocks.size() / 8 + 16);

    for (const auto& block : blocks) {
        const uint64_t leaf = block.oaddress.position;
        bool placed = false;
        for (int64_t depth = static_cast<int64_t>(height); depth >= 0; --depth) {
            uint64_t node = heap_node_for_path_bucket(
                leaf, static_cast<uint64_t>(depth), height);
            if (occupancy[node] >= ORAM__Z) continue;

            uint8_t slot = occupancy[node]++;
            auto root_depth =
                static_cast<_ORAM::Index>(depth)
                - (static_cast<_ORAM::Index>(depth)
                   % ORAM_SERVER__LEVELS_PER_PACK);
            auto root_idx =
                _ORAM::Indexers::GetHBIndex<ORAM_SERVER__LEVELS_PER_PACK>(
                    height, leaf, root_depth);
            auto inner_idx =
                _ORAM::Indexers::GetLBIndex<ORAM_SERVER__LEVELS_PER_PACK>(
                    height, leaf, static_cast<_ORAM::Index>(depth));
            auto [it, inserted] =
                large_buckets.emplace(root_idx, EnigLargeBucket::DUMMY());
            auto& bucket = it->second.buckets[inner_idx];
            bucket.blocks[slot] = block.block;
            bucket.md.addresses[slot] = block.oaddress;
            placed = true;
            break;
        }
        if (!placed) {
            overflow.push_back(block);
        }
    }

    auto& storage = path_oram.oramServerClient;
    for (const auto& [root_idx, large_bucket] : large_buckets) {
        storage.server.Write(root_idx, large_bucket);
    }

    if (overflow.size() > path_oram.S_) {
        throw std::runtime_error(
            "EnigMap bulk setup overflow exceeds Path ORAM stash bound");
    }
    path_oram.stash_ = std::move(overflow);
    path_oram.state.forcedIntoStash = false;
    path_oram.state.savedPath = _ORAM::DUMMY_POSITION;
}

static std::unique_ptr<EnigObst> build_enigmap_bulk(
    int capacity,
    const std::vector<std::pair<_OBST::K, _OBST::V>>& points) {
    int safe_capacity = std::max(capacity, 4);
    auto map = std::make_unique<EnigObst>(safe_capacity, true);

    map->oram.oram.stash_.clear();
    map->oram.oram.state.forcedIntoStash = false;
    map->oram.oram.state.savedPath = _ORAM::DUMMY_POSITION;
    map->count = points.size();
    map->oram.nextFreeBlock.address = points.size() + 2;

    map->FAKE_NODE = {0, map->oram.GenRandomPosition()};
    std::vector<EnigStashedBlock> real_nodes;
    real_nodes.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        EnigStashedBlock block = EnigStashedBlock::DUMMY();
        block.cached = false;
        block.oaddress = {
            static_cast<_ORAM::Address>(i + 2),
            map->oram.GenRandomPosition(),
        };
        block.block.data = _OBST::Node{
            points[i].first,
            points[i].second,
            {map->FAKE_NODE, map->FAKE_NODE},
            _OBST::B_BALANCED,
        };
        real_nodes.push_back(block);
    }

    _ORAM::ORAMAddress tree_root = map->FAKE_NODE;
    if (!real_nodes.empty()) {
        tree_root = build_balanced_obst_nodes(
            real_nodes, 0, real_nodes.size() - 1, map->FAKE_NODE).second;
    }

    map->root = {1, map->oram.GenRandomPosition()};
    EnigStashedBlock fake_block = EnigStashedBlock::DUMMY();
    fake_block.cached = false;
    fake_block.oaddress = map->FAKE_NODE;
    fake_block.block.data = _OBST::Node{
        _OBST::INVALID_KEY,
        _OBST::INVALID_VALUE,
        {map->FAKE_NODE, map->FAKE_NODE},
        _OBST::B_BALANCED,
    };

    EnigStashedBlock wrapper_root = EnigStashedBlock::DUMMY();
    wrapper_root.cached = false;
    wrapper_root.oaddress = map->root;
    wrapper_root.block.data = _OBST::Node{
        _OBST::INVALID_KEY,
        _OBST::INVALID_VALUE,
        {tree_root, map->FAKE_NODE},
        _OBST::B_BALANCED,
    };

    std::vector<EnigStashedBlock> all_blocks;
    all_blocks.reserve(real_nodes.size() + 2);
    all_blocks.push_back(fake_block);
    all_blocks.push_back(wrapper_root);
    all_blocks.insert(all_blocks.end(), real_nodes.begin(), real_nodes.end());
    place_blocks_in_enig_path_oram(*map, all_blocks);
    return map;
}

class OriginalEnigMapRefs {
public:
    OriginalEnigMapRefs(int capacity, int value_size)
        : capacity_(std::max(capacity, 4)),
          value_size_(value_size) {
        values_.push_back(Bytes(value_size_, 0));
    }

    void bulk_load_sorted(const std::vector<std::pair<int, Bytes>>& data) {
        values_.clear();
        values_.push_back(Bytes(value_size_, 0));
        std::vector<std::pair<_OBST::K, _OBST::V>> points;
        points.reserve(data.size());
        for (const auto& [key, value] : data) {
            _OBST::V ref = values_.size();
            values_.push_back(pad_bytes(value, value_size_));
            points.push_back({
                static_cast<_OBST::K>(key),
                ref,
            });
        }
        map_ = build_enigmap_bulk(capacity_, points);
    }

    void insert_or_update(int key, const Bytes& value) {
        if (!map_) {
            map_ = std::make_unique<EnigObst>(capacity_);
        }
        uint64_t ref = values_.size();
        values_.push_back(pad_bytes(value, value_size_));
        map_->Insert(static_cast<_OBST::K>(key), static_cast<_OBST::V>(ref));
    }

    Bytes get(int key, bool* found = nullptr) {
        _OBST::V ref = 0;
        bool ok = map_->Get(static_cast<_OBST::K>(key), ref);
        ok = ok && ref < values_.size();
        if (found) *found = ok;
        return ok ? values_[ref] : Bytes(value_size_, 0);
    }

private:
    int capacity_;
    int value_size_;
    std::unique_ptr<EnigObst> map_;
    std::vector<Bytes> values_;
};

struct PackedOriginalResult {
    Bytes value;
    bool found_in_hot = false;
    uint64_t hot_pages = 0;
};

class PackedOriginalEnigMap {
public:
    PackedOriginalEnigMap(int total_keys, int hot_keys, int value_size)
        : total_keys_(total_keys),
          hot_capacity_(hot_keys),
          value_size_(value_size),
          hot_dir_(std::make_unique<PackedDirectory>(hot_capacity_)),
          hot_oram_(std::make_unique<EnclaveOram>(
              std::max(hot_capacity_, 1), value_size_, 4, 7,
              EnclaveOramLayout::Veb)),
          cold_(std::make_unique<OriginalEnigMapRefs>(total_keys_, value_size_)) {}

    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys) {
        std::vector<int> is_hot_key(total_keys_, 0);
        for (int k : hot_keys) {
            if (0 <= k && k < total_keys_) is_hot_key[k] = 1;
        }

        std::vector<std::pair<int, Bytes>> hot_data;
        hot_data.reserve(hot_capacity_);
        std::vector<std::pair<int, Bytes>> cold_data;
        cold_data.reserve(std::max(0, total_keys_ - hot_capacity_));
        for (const auto& [k, v] : all_data) {
            if (0 <= k && k < total_keys_ && is_hot_key[k]) {
                hot_data.push_back({k, pad_bytes(v, value_size_)});
            } else {
                cold_data.push_back({k, v});
            }
        }
        cold_->bulk_load_sorted(cold_data);

        Bytes dummy(value_size_, 0);
        for (int i = static_cast<int>(hot_data.size()); i < hot_capacity_; ++i)
            hot_data.push_back({-(i + 1), dummy});

        auto leaves = hot_oram_->init(hot_data);
        for (const auto& [k, leaf] : leaves) {
            if (k >= 0) hot_dir_->insert(k, leaf);
        }
    }

    using ResponseCallback = std::function<void(const Bytes& value, bool found)>;

    PackedOriginalResult access(int key, bool tier_membership_mode,
                                ResponseCallback early_cb = nullptr) {
        PackedOriginalResult r;
        r.value.assign(value_size_, 0);

        int old_leaf = hot_dir_->lookup(key);
        int hot_hit_i = 1 - o_equal(old_leaf, INVALID_LEAF);
        r.found_in_hot = hot_hit_i != 0;
        int dir_pages = (hot_capacity_ * 20 + EnclaveOram::PAGE_SIZE - 1)
                      / EnclaveOram::PAGE_SIZE;

        bool skip_hot_oram = tier_membership_mode && !r.found_in_hot;
        if (!skip_hot_oram) {
            int new_leaf = hot_oram_->random_leaf();
            int dummy_leaf = hot_oram_->random_leaf();
            int use_leaf = o_select_i(hot_hit_i, old_leaf, dummy_leaf);

            hot_oram_->read_path_to_stash(use_leaf);
            Block blk = hot_oram_->extract_from_stash(
                o_select_i(hot_hit_i, key, -2));

            Bytes hot_val = pad_bytes(blk.value, value_size_);
            o_mov_bytes(hot_hit_i, r.value, hot_val);

            hot_oram_->add_to_stash(
                o_select_i(hot_hit_i, key, blk.key), new_leaf, hot_val);
            hot_oram_->evict_one_path(use_leaf);
            hot_dir_->update_pos(key, o_select_i(hot_hit_i, new_leaf, INVALID_LEAF));
        }
        r.hot_pages = static_cast<uint64_t>(dir_pages) * 2
                    + (skip_hot_oram ? 0 : hot_oram_->last_stats().pages_touched);

        if (early_cb)
            early_cb(r.value, r.found_in_hot);

        bool skip_cold = tier_membership_mode && r.found_in_hot;
        if (!skip_cold) {
            bool cold_found = false;
            Bytes cold_val = cold_->get(key, &cold_found);
            int use_cold = (1 - hot_hit_i) & (cold_found ? 1 : 0);
            o_mov_bytes(use_cold, r.value, cold_val);
        }

        return r;
    }

private:
    int total_keys_;
    int hot_capacity_;
    int value_size_;
    std::unique_ptr<PackedDirectory> hot_dir_;
    std::unique_ptr<EnclaveOram> hot_oram_;
    std::unique_ptr<OriginalEnigMapRefs> cold_;
};

static void run_all(const Config& cfg) {
    ensure_dir(cfg.outdir);
    auto profiles = make_profiles(cfg);

    std::ofstream csv(cfg.outdir + "/tee_original_enigmap.csv");
    csv << "profile,logN,N,n,s,value_size,Q,config,"
        << "measured_hot_us,measured_cold_us,measured_total_us,"
        << "measured_answer_us,modeled_total_us,hit_pct,"
        << "speedup_measured,speedup_answer_measured,speedup_modeled,"
        << "trusted_kb,page_us,hot_working_kb,cold_working_kb,"
        << "cold_pages_est,hot_pages_avg\n";

    std::cout << "=== Original EnigMap cold-map experiment ===\n";
    std::cout << "env=" << cfg.env
              << " trusted_kb=" << cfg.trusted_kb
              << " page_us=" << cfg.page_us << "\n";
    std::cout << std::setw(12) << "profile"
              << std::setw(6) << "logN"
              << std::setw(32) << "config"
              << std::setw(12) << "meas_us"
              << std::setw(12) << "model_us"
              << std::setw(9) << "spd_m"
              << std::setw(9) << "spd_mod"
              << std::setw(8) << "hit%"
              << "\n";

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        uint64_t backend_bytes = estimate_enig_backend_bytes(N);
        reset_enig_default_backend(backend_bytes);
        std::cout << "logN=" << logN
                  << " EnigMap backend="
                  << std::fixed << std::setprecision(2)
                  << (backend_bytes / 1024.0 / 1024.0 / 1024.0)
                  << " GiB required\n";
        auto stage_start = Clock::now();
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);
        log_stage(logN, "data_generation", stage_start);
        uint64_t hot_working_kb =
            (estimate_packed_hot_working_bytes(n, cfg.value_size) + 1023) / 1024;
        uint64_t cold_working_kb =
            (estimate_enigmap_working_bytes(N) + 1023) / 1024;
        double cold_pages = estimate_enigmap_query_pages(N);

        Measurement flat;
        {
            ZipfSampler zipf(N, cfg.s, 42);
            OriginalEnigMapRefs flat_map(N, cfg.value_size);
            stage_start = Clock::now();
            flat_map.bulk_load_sorted(data);
            log_stage(logN, "flat_bulk_setup", stage_start);

            stage_start = Clock::now();
            double sum_us = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                auto t0 = Clock::now();
                Bytes value = flat_map.get(key);
                auto t1 = Clock::now();
                verify_value(key, value);
                sum_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            }
            log_stage(logN, "flat_queries", stage_start);
            flat.config = "flat_original_enigmap";
            flat.flat = true;
            flat.hot_us = flat.cold_us = flat.total_us = sum_us / cfg.Q;
            flat.answer_us = flat.total_us;
            flat.hot_count = 0;
            flat.cold_count = cfg.Q;
        }

        std::vector<Measurement> packed_rows;
        for (bool tm : {false, true}) {
            PackedOriginalEnigMap packed(N, n, cfg.value_size);
            stage_start = Clock::now();
            packed.init(data, hk);
            log_stage(logN, tm ? "packed_TM_setup" : "packed_FO_setup",
                      stage_start);

            stage_start = Clock::now();
            double sum_hot = 0, sum_cold = 0, sum_total = 0, sum_answer = 0;
            double sum_all_hot_pages = 0, sum_hot_query_pages = 0;
            int hot_cnt = 0, cold_cnt = 0;
            ZipfSampler local_zipf(N, cfg.s, 42);
            for (int q = 0; q < cfg.Q; ++q) {
                int key = local_zipf.sample();
                Clock::time_point t_answer;
                bool answer_ready = false;
                auto t0 = Clock::now();
                auto r = packed.access(key, tm,
                    [&](const Bytes&, bool found) {
                        if (found) {
                            t_answer = Clock::now();
                            answer_ready = true;
                        }
                    });
                auto t1 = Clock::now();
                verify_value(key, r.value);
                double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                double answer_us = answer_ready
                    ? std::chrono::duration<double, std::micro>(
                          t_answer - t0).count()
                    : us;
                sum_total += us;
                sum_answer += answer_us;
                sum_all_hot_pages += r.hot_pages;
                if (r.found_in_hot) {
                    sum_hot += answer_us;
                    sum_hot_query_pages += r.hot_pages;
                    ++hot_cnt;
                } else {
                    sum_cold += us;
                    ++cold_cnt;
                }
            }
            log_stage(logN, tm ? "packed_TM_queries" : "packed_FO_queries",
                      stage_start);
            Measurement m;
            m.config = tm ? "packed_TM_original_enigmap"
                          : "packed_FO_original_enigmap";
            m.tier_membership = tm;
            m.hot_us = hot_cnt > 0 ? sum_hot / hot_cnt : 0;
            m.cold_us = cold_cnt > 0 ? sum_cold / cold_cnt : 0;
            m.total_us = sum_total / cfg.Q;
            m.answer_us = sum_answer / cfg.Q;
            m.hit_pct = 100.0 * hot_cnt / cfg.Q;
            m.hot_count = hot_cnt;
            m.cold_count = cold_cnt;
            m.avg_hot_pages = tm
                ? (hot_cnt > 0 ? sum_hot_query_pages / hot_cnt : 0)
                : (sum_all_hot_pages / cfg.Q);
            packed_rows.push_back(m);
        }

        for (const auto& profile : profiles) {
            double flat_model = modeled_total(
                flat, profile, N, n, cfg.value_size);

            auto write_row = [&](const Measurement& m) {
                double model = modeled_total(m, profile, N, n, cfg.value_size);
                double speed_meas = m.flat ? 1.0 : flat.total_us / m.total_us;
                double speed_answer = m.flat ? 1.0 : flat.answer_us / m.answer_us;
                double speed_model = m.flat ? 1.0 : flat_model / model;

                std::cout << std::setw(12) << profile.name
                          << std::setw(6) << logN
                          << std::setw(32) << m.config
                          << std::setw(12) << std::fixed << std::setprecision(1)
                          << m.total_us
                          << std::setw(12) << model
                          << std::setw(9) << std::setprecision(2) << speed_meas
                          << std::setw(9) << speed_model
                          << std::setw(8);
                if (m.hit_pct >= 0.0)
                    std::cout << std::setprecision(0) << m.hit_pct;
                else
                    std::cout << "-";
                std::cout << "\n";

                csv << profile.name << ","
                    << logN << "," << N << "," << n << ","
                    << std::fixed << std::setprecision(3) << cfg.s << ","
                    << cfg.value_size << "," << cfg.Q << ","
                    << m.config << ","
                    << std::setprecision(3)
                    << m.hot_us << "," << m.cold_us << "," << m.total_us << ","
                    << m.answer_us << ","
                    << model << "," << m.hit_pct << ","
                    << speed_meas << "," << speed_answer << ","
                    << speed_model << ","
                    << profile.trusted_kb << "," << profile.page_us << ","
                    << hot_working_kb << "," << cold_working_kb << ","
                    << cold_pages << "," << m.avg_hot_pages << "\n";
            };

            write_row(flat);
            for (const auto& row : packed_rows)
                write_row(row);
        }
        csv.flush();
    }
    std::cout << "  -> " << cfg.outdir << "/tee_original_enigmap.csv\n";
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    run_all(cfg);
    return 0;
}
