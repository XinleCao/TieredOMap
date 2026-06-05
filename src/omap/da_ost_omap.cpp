#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/tcp_channel.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace tiered_omap {

static double lambert_w(double x) {
    if (x < 0) return 0;
    double w = std::log(std::max(x, 1.0));
    for (int i = 0; i < 30; ++i) {
        double ew = std::exp(w);
        double denom = ew * (w + 1.0);
        if (std::abs(denom) < 1e-15) break;
        w -= (w * ew - x) / denom;
    }
    return w;
}

// ─── Constructor ────────────────────────────────────────────────────────────

DaOstOmap::DaOstOmap(int capacity, OdsTreeType tree_type,
                     int num_positions, int bucket_size, int bplus_order,
                     StorageCreator storage_creator)
    : capacity_(capacity),
      num_positions_(num_positions > 0 ? num_positions : capacity),
      tree_type_(tree_type),
      bucket_size_(bucket_size),
      bplus_order_(bplus_order),
      storage_creator_(std::move(storage_creator)),
      daoram_(num_positions_ > 0 ? num_positions_ : capacity,
              bucket_size, 20, 64, 64, 10, storage_creator_),
      avl_ods_(capacity, bucket_size, storage_creator_),
      bplus_ods_(capacity, bplus_order, bucket_size, storage_creator_) {

    double n = static_cast<double>(std::max(num_positions_, 2));
    double arg = std::exp(-1.0) * (std::log2(n) + 127.0);
    double w = lambert_w(arg);
    int tree_size = static_cast<int>(std::ceil(std::exp(w + 1.0)));
    tree_height_bound_ = std::max(1,
        static_cast<int>(std::ceil(1.44 * std::log2(std::max(tree_size, 2)))));

    if (tree_type_ == OdsTreeType::AVL) {
        ods_budget_ = 3 * tree_height_bound_;
        avl_ods_.set_ods_mode(tree_height_bound_);
    } else {
        ods_budget_ = 3 * tree_height_bound_;
        bplus_ods_.set_ods_mode(tree_height_bound_);
    }

    hash_seed_ = static_cast<uint64_t>(std::random_device{}());
}

// ─── Hashing ────────────────────────────────────────────────────────────────

int DaOstOmap::hash_to_position(int key) const {
    std::hash<uint64_t> h;
    uint64_t v = hash_seed_;
    v ^= h(static_cast<uint64_t>(key)) + 0x9e3779b9 + (v << 6) + (v >> 2);
    return static_cast<int>(v % static_cast<uint64_t>(num_positions_));
}

Bytes DaOstOmap::encode_root(int key, int leaf) {
    Bytes b(2 * sizeof(int));
    std::memcpy(b.data(), &key, sizeof(int));
    std::memcpy(b.data() + sizeof(int), &leaf, sizeof(int));
    return b;
}

std::pair<int,int> DaOstOmap::decode_root(const Bytes& b) {
    int key = INVALID_KEY, leaf = INVALID_LEAF;
    if (b.size() >= 2 * sizeof(int)) {
        std::memcpy(&key, b.data(), sizeof(int));
        std::memcpy(&leaf, b.data() + sizeof(int), sizeof(int));
    }
    return {key, leaf};
}

// ─── Padding & bandwidth ───────────────────────────────────────────────────

void DaOstOmap::pad_ods(int actual_ops) {
    int pad = std::max(0, ods_budget_ - actual_ops);
    if (tree_type_ == OdsTreeType::AVL) {
        for (int i = 0; i < pad; ++i)
            avl_ods_.oram().dummy_access();
    } else {
        for (int i = 0; i < pad; ++i)
            bplus_ods_.oram().dummy_access();
    }
}

void DaOstOmap::finalize_bw(int ods_ops) {
    int da_rounds = daoram_.num_access_rounds();
    int total_ops = ods_ops + da_rounds;
    PathORAM& ods = (tree_type_ == OdsTreeType::AVL)
                        ? avl_ods_.oram() : bplus_ods_.oram();
    int da_bw = daoram_.last_stats().bytes_downloaded +
                daoram_.last_stats().bytes_uploaded;
    int ods_bw = ods.path_bandwidth_bytes();
    last_bw_.rounds = total_ops;
    last_bw_.bytes_downloaded = da_bw / 2 +
        static_cast<uint64_t>(ods_ops) * ods_bw;
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;
}

void DaOstOmap::set_round_delay_us(int us) {
    if (tree_type_ == OdsTreeType::AVL)
        avl_ods_.set_round_delay_us(us);
    else
        bplus_ods_.set_round_delay_us(us);
}

// ─── AVL multi-tree init ────────────────────────────────────────────────────

void DaOstOmap::do_avl_init(const std::vector<std::pair<int, Bytes>>& data) {
    std::vector<std::vector<std::pair<int, Bytes>>> buckets(num_positions_);
    for (auto& [k, v] : data)
        buckets[hash_to_position(k)].emplace_back(k, v);

    std::unordered_map<int, AVLNodeData> all_nodes;
    std::vector<int> roots(num_positions_, INVALID_KEY);

    for (int pos = 0; pos < num_positions_; ++pos) {
        if (buckets[pos].empty()) continue;
        auto& items = buckets[pos];
        std::sort(items.begin(), items.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });

        std::function<int(int, int)> build = [&](int lo, int hi) -> int {
            if (lo > hi) return INVALID_KEY;
            int mid = lo + (hi - lo) / 2;
            int user_key = items[mid].first;
            AVLNodeData nd;
            nd.data = items[mid].second;
            nd.l_key = build(lo, mid - 1);
            nd.r_key = build(mid + 1, hi);
            all_nodes[user_key] = nd;
            return user_key;
        };
        roots[pos] = build(0, static_cast<int>(items.size()) - 1);
    }

    std::function<int(int)> fix_h = [&](int key) -> int {
        if (key == INVALID_KEY) return 0;
        auto& nd = all_nodes.at(key);
        nd.l_height = fix_h(nd.l_key);
        nd.r_height = fix_h(nd.r_key);
        return nd.height();
    };
    for (int pos = 0; pos < num_positions_; ++pos)
        if (roots[pos] != INVALID_KEY) fix_h(roots[pos]);

    PathORAM& ods = avl_ods_.oram();
    ods = PathORAM(capacity_, bucket_size_, 7, storage_creator_);
    for (auto& [k, _] : all_nodes)
        ods.set_leaf(k, ods.random_leaf());

    for (auto& [k, nd] : all_nodes) {
        if (nd.l_key != INVALID_KEY) nd.l_leaf = ods.get_leaf(nd.l_key);
        if (nd.r_key != INVALID_KEY) nd.r_leaf = ods.get_leaf(nd.r_key);
    }

    std::unordered_map<int, Bytes> oram_data;
    for (auto& [k, nd] : all_nodes)
        oram_data[k] = nd.encode();
    ods.init(oram_data);

    std::unordered_map<int, Bytes> da_data;
    for (int pos = 0; pos < num_positions_; ++pos) {
        int rk = roots[pos];
        int rl = (rk != INVALID_KEY) ? ods.get_leaf(rk) : INVALID_LEAF;
        da_data[pos] = encode_root(rk, rl);
        root_cache_[pos] = {rk, rl};
    }
    daoram_.init(da_data);
}

// ─── B+ tree multi-tree init ────────────────────────────────────────────────

void DaOstOmap::do_bplus_init(const std::vector<std::pair<int, Bytes>>& data) {
    std::vector<std::vector<std::pair<int, Bytes>>> buckets(num_positions_);
    for (auto& [k, v] : data)
        buckets[hash_to_position(k)].emplace_back(k, v);

    PathORAM& ods = bplus_ods_.oram();
    ods = PathORAM(capacity_, bucket_size_, 7, storage_creator_);
    int next_id = 0;

    std::unordered_map<int, Bytes> oram_data;
    std::vector<int> root_ids(num_positions_, INVALID_KEY);

    for (int pos = 0; pos < num_positions_; ++pos) {
        if (buckets[pos].empty()) continue;
        auto& items = buckets[pos];
        std::sort(items.begin(), items.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });

        std::function<int(int, int)> build = [&](int lo, int hi) -> int {
            if (lo > hi) return INVALID_KEY;
            int n = hi - lo + 1;
            if (n <= bplus_order_ - 1) {
                BPlusNode leaf;
                leaf.is_leaf = true;
                for (int i = lo; i <= hi; ++i) {
                    leaf.keys.push_back(items[i].first);
                    leaf.values.push_back(items[i].second);
                }
                int id = next_id++;
                ods.set_leaf(id, ods.random_leaf());
                oram_data[id] = leaf.encode();
                return id;
            }

            int chunk = bplus_order_ - 1;
            std::vector<int> child_ids_vec;
            std::vector<int> sep_keys;
            int cur = lo;
            while (cur <= hi) {
                int end = std::min(cur + chunk - 1, hi);
                int cid = build(cur, end);
                child_ids_vec.push_back(cid);
                if (cur > lo) sep_keys.push_back(items[cur].first);
                cur = end + 1;
            }

            BPlusNode internal;
            internal.is_leaf = false;
            internal.keys = sep_keys;
            for (int cid : child_ids_vec) {
                internal.child_ids.push_back(cid);
                internal.child_leaves.push_back(ods.get_leaf(cid));
            }
            int id = next_id++;
            ods.set_leaf(id, ods.random_leaf());
            oram_data[id] = internal.encode();
            return id;
        };

        root_ids[pos] = build(0, static_cast<int>(items.size()) - 1);
    }

    ods.init(oram_data);
    bplus_ods_.set_next_block_id(next_id);

    std::unordered_map<int, Bytes> da_data;
    for (int pos = 0; pos < num_positions_; ++pos) {
        int rk = root_ids[pos];
        int rl = (rk != INVALID_KEY) ? ods.get_leaf(rk) : INVALID_LEAF;
        da_data[pos] = encode_root(rk, rl);
        root_cache_[pos] = {rk, rl};
    }
    daoram_.init(da_data);
}

// ─── Init ───────────────────────────────────────────────────────────────────

void DaOstOmap::init(const std::vector<std::pair<int, Bytes>>& data) {
    root_cache_.clear();
    if (tree_type_ == OdsTreeType::AVL)
        do_avl_init(data);
    else
        do_bplus_init(data);
}

// ─── Search ─────────────────────────────────────────────────────────────────

Bytes DaOstOmap::search(int key, const Bytes* update) {
    last_bw_.reset();

    int pos = hash_to_position(key);
    auto [rk, rl] = root_cache_[pos];

    // DAORAM access for obliviousness only; ignore returned value.
    daoram_.access(pos);

    Bytes result;
    int ods_ops = 0;

    if (tree_type_ == OdsTreeType::AVL) {
        avl_ods_.set_root(rk, rl);
        result = avl_ods_.search(key, update);
        ods_ops = avl_ods_.last_op_count();
        auto [nrk, nrl] = avl_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    } else {
        bplus_ods_.set_root(rk, rl);
        result = bplus_ods_.search(key, update);
        ods_ops = bplus_ods_.last_op_count();
        auto [nrk, nrl] = bplus_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    }

    pad_ods(ods_ops);
    finalize_bw(ods_budget_);
    return result;
}

// ─── Insert ─────────────────────────────────────────────────────────────────

void DaOstOmap::insert(int key, const Bytes& value) {
    last_bw_.reset();

    int pos = hash_to_position(key);
    auto [rk, rl] = root_cache_[pos];

    daoram_.access(pos);

    int ods_ops = 0;

    if (tree_type_ == OdsTreeType::AVL) {
        avl_ods_.set_root(rk, rl);
        avl_ods_.insert(key, value);
        ods_ops = avl_ods_.last_op_count();
        auto [nrk, nrl] = avl_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    } else {
        bplus_ods_.set_root(rk, rl);
        bplus_ods_.insert(key, value);
        ods_ops = bplus_ods_.last_op_count();
        auto [nrk, nrl] = bplus_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    }

    pad_ods(ods_ops);
    finalize_bw(ods_budget_);
}

// ─── Remove ─────────────────────────────────────────────────────────────────

void DaOstOmap::remove(int key) {
    last_bw_.reset();

    int pos = hash_to_position(key);
    auto [rk, rl] = root_cache_[pos];

    daoram_.access(pos);

    int ods_ops = 0;

    if (tree_type_ == OdsTreeType::AVL) {
        avl_ods_.set_root(rk, rl);
        avl_ods_.remove(key);
        ods_ops = avl_ods_.last_op_count();
        auto [nrk, nrl] = avl_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    } else {
        bplus_ods_.set_root(rk, rl);
        bplus_ods_.remove(key);
        ods_ops = bplus_ods_.last_op_count();
        auto [nrk, nrl] = bplus_ods_.get_root();
        root_cache_[pos] = {nrk, nrl};
    }

    pad_ods(ods_ops);
    finalize_bw(ods_budget_);
}

// ─── Piggyback scan ─────────────────────────────────────────────────────────

DaOstOmap::ScanResult DaOstOmap::piggyback_scan_step() {
    ScanResult sr;
    last_bw_.reset();

    int pos = scan_pos_;
    int rk = INVALID_KEY, rl = INVALID_LEAF;
    auto it = root_cache_.find(pos);
    if (it != root_cache_.end()) {
        rk = it->second.first;
        rl = it->second.second;
    }

    daoram_.access(pos);

    int ods_ops = 0;
    if (rk != INVALID_KEY) {
        if (tree_type_ == OdsTreeType::AVL) {
            avl_ods_.set_root(rk, rl);
            sr.value = avl_ods_.search(rk);
            ods_ops = avl_ods_.last_op_count();
            auto [nrk, nrl] = avl_ods_.get_root();
            root_cache_[pos] = {nrk, nrl};
        } else {
            bplus_ods_.set_root(rk, rl);
            sr.value = bplus_ods_.search(rk);
            ods_ops = bplus_ods_.last_op_count();
            auto [nrk, nrl] = bplus_ods_.get_root();
            root_cache_[pos] = {nrk, nrl};
        }
        sr.key = rk;
    }

    pad_ods(ods_ops);
    finalize_bw(ods_budget_);

    scan_pos_ = (scan_pos_ + 1) % num_positions_;
    return sr;
}

// ─── Dummy ──────────────────────────────────────────────────────────────────

void DaOstOmap::dummy_access() {
    last_bw_.reset();

    daoram_.dummy_access();

    if (tree_type_ == OdsTreeType::AVL) {
        for (int i = 0; i < ods_budget_; ++i)
            avl_ods_.oram().dummy_access();
    } else {
        for (int i = 0; i < ods_budget_; ++i)
            bplus_ods_.oram().dummy_access();
    }

    finalize_bw(ods_budget_);
}

void DaOstOmap::partial_dummy_access() {
    last_bw_.reset();

    daoram_.dummy_access();
    ods_oram().dummy_access();

    finalize_bw(1);
}

// ─── Step-by-step interface for interleaved access ─────────────────────────

OmapInterface& DaOstOmap::ods_omap() {
    return (tree_type_ == OdsTreeType::AVL)
               ? static_cast<OmapInterface&>(avl_ods_)
               : static_cast<OmapInterface&>(bplus_ods_);
}

const OmapInterface& DaOstOmap::ods_omap() const {
    return (tree_type_ == OdsTreeType::AVL)
               ? static_cast<const OmapInterface&>(avl_ods_)
               : static_cast<const OmapInterface&>(bplus_ods_);
}

PathORAM& DaOstOmap::ods_oram() {
    return (tree_type_ == OdsTreeType::AVL)
               ? avl_ods_.oram() : bplus_ods_.oram();
}

void DaOstOmap::begin_step_search(int key, const Bytes* update) {
    last_bw_.reset();
    ss_ = StepState{};
    ss_.key = key;
    ss_.update = update;
    ss_.pos = hash_to_position(key);
    auto [rk, rl] = root_cache_[ss_.pos];

    daoram_.begin_step_access(ss_.pos);

    if (tree_type_ == OdsTreeType::AVL) {
        avl_ods_.set_root(rk, rl);
        avl_ods_.begin_step_search(key, update);
    } else {
        bplus_ods_.set_root(rk, rl);
        bplus_ods_.begin_step_search(key, update);
    }
    ss_.phase = StepPhase::DAORAM;
}

void DaOstOmap::begin_step_dummy() {
    last_bw_.reset();
    ss_ = StepState{};
    ss_.is_dummy = true;

    daoram_.begin_step_dummy();

    if (tree_type_ == OdsTreeType::AVL)
        avl_ods_.begin_step_dummy();
    else
        bplus_ods_.begin_step_dummy();

    ss_.phase = StepPhase::DAORAM;
}

void DaOstOmap::begin_step_partial_dummy() {
    last_bw_.reset();
    ss_ = StepState{};
    ss_.is_dummy = true;
    ss_.is_partial_dummy = true;

    daoram_.begin_step_dummy();
    ss_.phase = StepPhase::DAORAM;
}

void DaOstOmap::begin_step_scan() {
    last_bw_.reset();
    ss_ = StepState{};
    ss_.is_scan = true;
    ss_.scan_pos = scan_pos_;

    auto it = root_cache_.find(ss_.scan_pos);
    if (it != root_cache_.end()) {
        ss_.scan_root_key = it->second.first;
        ss_.scan_root_leaf = it->second.second;
    }

    daoram_.begin_step_access(ss_.scan_pos);
    ss_.phase = StepPhase::SCAN_DAORAM;
}

DaOstOmap::ScanResult DaOstOmap::step_finish_scan() {
    ScanResult sr;
    sr.key = ss_.scan_root_key;
    sr.value = ss_.scan_result;

    scan_pos_ = (scan_pos_ + 1) % num_positions_;
    finalize_bw(ods_budget_);
    return sr;
}

OramStepRound DaOstOmap::step_next_round() {
    OramStepRound r;
    if (ss_.phase == StepPhase::DONE) return r;

    ss_.round_phase = ss_.phase;

    if (ss_.phase == StepPhase::DAORAM || ss_.phase == StepPhase::SCAN_DAORAM) {
        auto reads = daoram_.step_get_reads();
        r.reads.insert(r.reads.end(), reads.begin(), reads.end());
    } else if (ss_.phase == StepPhase::ODS || ss_.phase == StepPhase::SCAN_ODS) {
        return ods_omap().step_next_round();
    } else {
        ss_.pad_round_leaf = ods_oram().random_leaf();
        r.reads.push_back({ods_oram().get_store_id(), ss_.pad_round_leaf});
    }
    return r;
}

void DaOstOmap::step_apply_reads(const std::vector<PathData>& results) {
    if (ss_.round_phase == StepPhase::DAORAM || ss_.round_phase == StepPhase::SCAN_DAORAM) {
        daoram_.step_apply_reads(results);
    } else if (ss_.round_phase == StepPhase::ODS || ss_.round_phase == StepPhase::SCAN_ODS) {
        ods_omap().step_apply_reads(results);
    } else if (ss_.round_phase == StepPhase::ODS_PAD || ss_.round_phase == StepPhase::SCAN_ODS_PAD) {
        if (!results.empty())
            ods_oram().apply_fetched_path(
                std::unordered_map<int, std::vector<Block>>(results[0]));
    }
}

void DaOstOmap::step_process() {
    if (ss_.round_phase == StepPhase::DAORAM) {
        daoram_.step_process();
        if (daoram_.step_done()) {
            daoram_.step_finish(nullptr);

            if (pb_.active && !pb_.is_dummy) {
                daoram_.piggyback_finish(nullptr);
                auto it = root_cache_.find(pb_.pos);
                int pb_rk = INVALID_KEY, pb_rl = INVALID_LEAF;
                if (it != root_cache_.end()) {
                    pb_rk = it->second.first;
                    pb_rl = it->second.second;
                }
                pb_.saved_main_root_key = root_cache_.count(ss_.pos)
                    ? root_cache_[ss_.pos].first : INVALID_KEY;
                pb_.saved_main_root_leaf = root_cache_.count(ss_.pos)
                    ? root_cache_[ss_.pos].second : INVALID_LEAF;
                if (tree_type_ == OdsTreeType::AVL) {
                    avl_ods_.set_root(pb_rk, pb_rl);
                    if (pb_.is_insert)
                        avl_ods_.begin_piggyback_insert(pb_.key, pb_.insert_value);
                    else
                        avl_ods_.begin_piggyback_search(pb_.key);
                } else {
                    bplus_ods_.set_root(pb_rk, pb_rl);
                    if (pb_.is_insert)
                        bplus_ods_.begin_piggyback_insert(pb_.key, pb_.insert_value);
                    else
                        bplus_ods_.begin_piggyback_search(pb_.key);
                }
            } else if (pb_.active && pb_.is_dummy) {
                daoram_.piggyback_finish_dummy();
                if (tree_type_ == OdsTreeType::AVL)
                    avl_ods_.begin_piggyback_dummy();
                else
                    bplus_ods_.begin_piggyback_dummy();
            }

            if (ss_.is_partial_dummy) {
                ss_.pad_remaining = 1;
                ss_.phase = StepPhase::ODS_PAD;
            } else {
                ss_.phase = StepPhase::ODS;
            }
        }
    } else if (ss_.round_phase == StepPhase::SCAN_DAORAM) {
        daoram_.step_process();
        if (daoram_.step_done()) {
            daoram_.step_finish(nullptr);
            if (ss_.scan_root_key != INVALID_KEY) {
                if (tree_type_ == OdsTreeType::AVL) {
                    avl_ods_.set_root(ss_.scan_root_key, ss_.scan_root_leaf);
                    avl_ods_.begin_step_search(ss_.scan_root_key, nullptr);
                } else {
                    bplus_ods_.set_root(ss_.scan_root_key, ss_.scan_root_leaf);
                    bplus_ods_.begin_step_search(ss_.scan_root_key, nullptr);
                }
                ss_.phase = StepPhase::SCAN_ODS;
            } else {
                ss_.pad_remaining = ods_budget_;
                ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::SCAN_ODS_PAD
                                                    : StepPhase::DONE;
            }
        }
    } else if (ss_.round_phase == StepPhase::ODS) {
        ods_omap().step_process();
        if (ods_omap().step_needs_decision()) {
            // Inner ODS paused for decision; DaOstOmap stays in ODS phase.
            // Caller detects via DaOstOmap::step_needs_decision().
        } else if (ods_omap().step_done()) {
            ss_.ods_ops = (tree_type_ == OdsTreeType::AVL)
                              ? avl_ods_.last_op_count()
                              : bplus_ods_.last_op_count();
            ss_.pad_remaining = std::max(0, ods_budget_ - ss_.ods_ops);
            ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::ODS_PAD
                                                : StepPhase::DONE;
        }
    } else if (ss_.round_phase == StepPhase::SCAN_ODS) {
        ods_omap().step_process();
        if (ods_omap().step_done()) {
            ss_.ods_ops = (tree_type_ == OdsTreeType::AVL)
                              ? avl_ods_.last_op_count()
                              : bplus_ods_.last_op_count();
            ss_.pad_remaining = std::max(0, ods_budget_ - ss_.ods_ops);
            ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::SCAN_ODS_PAD
                                                : StepPhase::DONE;
        }
    } else if (ss_.round_phase == StepPhase::ODS_PAD || ss_.round_phase == StepPhase::SCAN_ODS_PAD) {
        ss_.pad_remaining--;
        if (ss_.pad_remaining <= 0)
            ss_.phase = StepPhase::DONE;
    }
}

std::vector<StepWriteReq> DaOstOmap::step_prepare_writes() {
    if (ss_.round_phase == StepPhase::DAORAM || ss_.round_phase == StepPhase::SCAN_DAORAM) {
        return daoram_.step_get_writes();
    }
    if (ss_.round_phase == StepPhase::ODS || ss_.round_phase == StepPhase::SCAN_ODS) {
        return ods_omap().step_prepare_writes();
    }
    if (ss_.round_phase == StepPhase::ODS_PAD || ss_.round_phase == StepPhase::SCAN_ODS_PAD) {
        return {{ods_oram().get_store_id(),
                 ods_oram().prepare_eviction(ss_.pad_round_leaf)}};
    }
    return {};
}

bool DaOstOmap::step_done() const {
    return ss_.phase == StepPhase::DONE;
}

Bytes DaOstOmap::step_finish() {
    if (ss_.is_scan) {
        if (ss_.scan_root_key != INVALID_KEY) {
            ss_.scan_result = ods_omap().step_finish();
            auto [nrk, nrl] = (tree_type_ == OdsTreeType::AVL)
                                   ? avl_ods_.get_root()
                                   : bplus_ods_.get_root();
            root_cache_[ss_.scan_pos] = {nrk, nrl};
        }
        return {};
    }

    if (!ss_.is_dummy) {
        if (tree_type_ == OdsTreeType::AVL) {
            ss_.result = avl_ods_.step_finish();
            if (pb_.active) {
                root_cache_[ss_.pos] = {pb_.saved_main_root_key,
                                        pb_.saved_main_root_leaf};
            } else {
                auto [nrk, nrl] = avl_ods_.get_root();
                root_cache_[ss_.pos] = {nrk, nrl};
            }
        } else {
            ss_.result = bplus_ods_.step_finish();
            if (pb_.active) {
                root_cache_[ss_.pos] = {pb_.saved_main_root_key,
                                        pb_.saved_main_root_leaf};
            } else {
                auto [nrk, nrl] = bplus_ods_.get_root();
                root_cache_[ss_.pos] = {nrk, nrl};
            }
        }
    } else if (!ss_.is_partial_dummy) {
        ods_omap().step_finish();
    }

    finalize_bw(ss_.is_partial_dummy ? 1 : ods_budget_);
    return ss_.result;
}

void DaOstOmap::step_abort() {
    if (!ss_.is_partial_dummy)
        ods_omap().step_abort();
    if (ss_.phase == StepPhase::ODS || ss_.phase == StepPhase::ODS_PAD) {
        auto [nrk, nrl] = (tree_type_ == OdsTreeType::AVL)
                              ? avl_ods_.get_root()
                              : bplus_ods_.get_root();
        root_cache_[ss_.pos] = {nrk, nrl};
    }
    finalize_bw(ss_.ods_ops);
    ss_.phase = StepPhase::DONE;
    pb_ = PBState{};
}

// ─── Mid-access decision interface ─────────────────────────────────────────

void DaOstOmap::set_step_decision_enabled(bool enable) {
    ods_omap().set_step_decision_enabled(enable);
}

bool DaOstOmap::step_needs_decision() const {
    return ss_.phase == StepPhase::ODS && ods_omap().step_needs_decision();
}

Bytes DaOstOmap::step_get_traverse_result() {
    return ods_omap().step_get_traverse_result();
}

void DaOstOmap::step_commit_remove() {
    ods_omap().step_commit_remove();
}

void DaOstOmap::step_commit_noop() {
    ods_omap().step_commit_noop();
}

// ─── Piggyback interface ───────────────────────────────────────────────────

void DaOstOmap::begin_piggyback_insert(int key, const Bytes& value) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.is_insert = true;
    pb_.key = key;
    pb_.insert_value = value;
    pb_.pos = hash_to_position(key);
    daoram_.begin_piggyback_access(pb_.pos);
}

void DaOstOmap::begin_piggyback_search(int key) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.key = key;
    pb_.pos = hash_to_position(key);
    daoram_.begin_piggyback_access(pb_.pos);
}

void DaOstOmap::begin_piggyback_dummy() {
    pb_ = PBState{};
    pb_.active = true;
    pb_.is_dummy = true;
    daoram_.begin_piggyback_dummy();
}

Bytes DaOstOmap::finish_piggyback() {
    if (!pb_.active) return {};

    if (!pb_.is_dummy) {
        Bytes pb_result;
        if (tree_type_ == OdsTreeType::AVL) {
            pb_result = avl_ods_.finish_piggyback();
            auto [nrk, nrl] = avl_ods_.get_root();
            root_cache_[pb_.pos] = {nrk, nrl};
        } else {
            pb_result = bplus_ods_.finish_piggyback();
            auto [nrk, nrl] = bplus_ods_.get_root();
            root_cache_[pb_.pos] = {nrk, nrl};
        }
        pb_.result = std::move(pb_result);
    }

    Bytes result = std::move(pb_.result);
    pb_.active = false;
    return result;
}

// ─── State export / import ─────────────────────────────────────────────────

Bytes DaOstOmap::export_state() const {
    Bytes buf;
    auto si = [&](int v){ size_t p=buf.size(); buf.resize(p+4); std::memcpy(buf.data()+p,&v,4); };

    si(capacity_); si(num_positions_); si(tree_height_bound_); si(ods_budget_);
    si(static_cast<int>(tree_type_)); si(bucket_size_); si(bplus_order_);
    {uint64_t s=hash_seed_; buf.insert(buf.end(),(uint8_t*)&s,(uint8_t*)&s+8);}

    // root_cache
    si(static_cast<int>(root_cache_.size()));
    for (auto& [pos, kv] : root_cache_) { si(pos); si(kv.first); si(kv.second); }

    // DAOram state
    auto da_blob = daoram_.export_state(-1);
    si(static_cast<int>(da_blob.size()));
    buf.insert(buf.end(), da_blob.begin(), da_blob.end());

    // ODS tree state (AVL or BPlus)
    if (tree_type_ == OdsTreeType::AVL) {
        auto ods_blob = avl_ods_.export_state();
        si(static_cast<int>(ods_blob.size()));
        buf.insert(buf.end(), ods_blob.begin(), ods_blob.end());
    } else {
        auto ods_blob = bplus_ods_.export_state();
        si(static_cast<int>(ods_blob.size()));
        buf.insert(buf.end(), ods_blob.begin(), ods_blob.end());
    }
    return buf;
}

std::unique_ptr<DaOstOmap> DaOstOmap::from_state(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v,p,4); p+=4; return v; };

    int capacity = di(), num_positions = di(), tree_height_bound = di(),
        ods_budget = di(), tree_type_i = di(), bucket_size = di(),
        bplus_order = di();
    uint64_t hash_seed;
    std::memcpy(&hash_seed, p, 8); p += 8;

    auto tree_type = static_cast<OdsTreeType>(tree_type_i);

    int rc_sz = di();
    std::unordered_map<int, std::pair<int,int>> root_cache;
    for (int i = 0; i < rc_sz; ++i) {
        int pos = di(), k = di(), l = di();
        root_cache[pos] = {k, l};
    }

    int da_len = di(); (void)da_len;
    auto daoram = DAOram::from_state_network(p, channel);

    int ods_len = di(); (void)ods_len;

    auto obj = std::unique_ptr<DaOstOmap>(new DaOstOmap(
        capacity, tree_type, num_positions, bucket_size, bplus_order, nullptr));
    obj->hash_seed_ = hash_seed;
    obj->tree_height_bound_ = tree_height_bound;
    obj->ods_budget_ = ods_budget;
    obj->root_cache_ = std::move(root_cache);
    obj->daoram_ = std::move(daoram);

    if (tree_type == OdsTreeType::AVL) {
        auto avl = AVLOmap::from_state(p, channel);
        obj->avl_ods_ = std::move(*avl);
    } else {
        auto bp = BPlusOmap::from_state(p, channel);
        obj->bplus_ods_ = std::move(*bp);
    }
    return obj;
}

}  // namespace tiered_omap
