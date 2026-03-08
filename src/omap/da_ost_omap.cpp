#include "tiered_omap/omap/da_ost_omap.h"
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
              bucket_size, 20, 64, 64, storage_creator_),
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
        ods_budget_ = 2 * tree_height_bound_;
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

void DaOstOmap::finalize_bw(int /*ods_ops*/) {
    int total_ops = ods_budget_ + 1;
    PathORAM& ods = (tree_type_ == OdsTreeType::AVL)
                        ? avl_ods_.oram() : bplus_ods_.oram();
    int da_bw = daoram_.last_stats().bytes_downloaded +
                daoram_.last_stats().bytes_uploaded;
    int ods_bw = ods.path_bandwidth_bytes();
    last_bw_.rounds = total_ops;
    last_bw_.bytes_downloaded = da_bw / 2 +
        static_cast<uint64_t>(ods_budget_) * ods_bw;
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
    finalize_bw(ods_ops);
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
    finalize_bw(ods_ops);
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
    finalize_bw(ods_ops);
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
    finalize_bw(ods_ops);

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

    finalize_bw(0);
}

}  // namespace tiered_omap
