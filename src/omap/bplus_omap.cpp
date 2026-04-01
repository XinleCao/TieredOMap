#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/tcp_channel.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tiered_omap {

// ─── BPlusNode encoding ────────────────────────────────────────────────────

Bytes BPlusNode::encode() const {
    Bytes buf;
    uint8_t type = is_leaf ? (is_index_leaf ? 2 : 1) : 0;
    buf.push_back(type);

    auto push_int = [&](int v) {
        buf.resize(buf.size() + sizeof(int));
        std::memcpy(&buf[buf.size() - sizeof(int)], &v, sizeof(int));
    };

    push_int(static_cast<int>(keys.size()));
    for (int k : keys) push_int(k);

    if (type == 0) {
        for (size_t i = 0; i < child_ids.size(); ++i) {
            push_int(child_ids[i]);
            push_int(child_leaves[i]);
        }
    } else if (type == 2) {
        for (size_t i = 0; i < child_ids.size(); ++i)
            push_int(child_ids[i]);
    } else {
        for (auto& v : values) {
            push_int(static_cast<int>(v.size()));
            buf.insert(buf.end(), v.begin(), v.end());
        }
    }
    return buf;
}

BPlusNode BPlusNode::decode(const Bytes& raw) {
    BPlusNode nd;
    if (raw.empty()) return nd;

    size_t pos = 0;
    uint8_t type = raw[pos++];
    nd.is_leaf = (type != 0);
    nd.is_index_leaf = (type == 2);

    auto get_int = [&]() {
        int v;
        std::memcpy(&v, &raw[pos], sizeof(int));
        pos += sizeof(int);
        return v;
    };

    int nk = get_int();
    nd.keys.resize(nk);
    for (int i = 0; i < nk; ++i)
        nd.keys[i] = get_int();

    if (type == 0) {
        int nc = nk + 1;
        nd.child_ids.resize(nc);
        nd.child_leaves.resize(nc);
        for (int i = 0; i < nc; ++i) {
            nd.child_ids[i] = get_int();
            nd.child_leaves[i] = get_int();
        }
    } else if (type == 2) {
        nd.child_ids.resize(nk);
        for (int i = 0; i < nk; ++i)
            nd.child_ids[i] = get_int();
    } else {
        nd.values.resize(nk);
        for (int i = 0; i < nk; ++i) {
            int sz = get_int();
            nd.values[i].assign(raw.begin() + pos, raw.begin() + pos + sz);
            pos += sz;
        }
    }
    return nd;
}

// ─── BPlusOmap ──────────────────────────────────────────────────────────────

BPlusOmap::BPlusOmap(int capacity, int order, int bucket_size,
                     StorageCreator storage_creator)
    : order_(order),
      storage_creator_(std::move(storage_creator)),
      oram_(capacity, bucket_size, 7, storage_creator_) {
    int half = std::max(static_cast<int>(std::ceil(order / 2.0)), 2);
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(std::log(std::max(capacity, 2))
                                   / std::log(half))) + 1);
}

BPlusOmap::BPlusOmap(int capacity, int order, int bucket_size,
                     int split_depth, int upper_capacity,
                     StorageCreator storage_creator)
    : order_(order),
      split_depth_(split_depth),
      storage_creator_(std::move(storage_creator)),
      upper_oram_(std::max(upper_capacity, 1), bucket_size, 7, storage_creator_),
      oram_(capacity, bucket_size, 7, storage_creator_) {
    int half = std::max(static_cast<int>(std::ceil(order / 2.0)), 2);
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(std::log(std::max(capacity, 2))
                                   / std::log(half))) + 1);
    if (split_depth_ > max_height_)
        split_depth_ = max_height_;
}

PathORAM& BPlusOmap::oram_for_depth(int depth) {
    return (split_depth_ > 0 && depth < split_depth_) ? upper_oram_ : oram_;
}

// ─── Build B+ tree bottom-up ────────────────────────────────────────────────

int BPlusOmap::build_tree(
    const std::vector<std::pair<int, Bytes>>& sorted,
    std::unordered_map<int, Bytes>& oram_data) {
    if (sorted.empty()) return INVALID_KEY;

    std::vector<std::pair<int, int>> level_ids;
    int mlk = max_leaf_keys();

    if (index_mode_) {
        for (size_t i = 0; i < sorted.size(); ) {
            BPlusNode ileaf;
            ileaf.is_leaf = true;
            ileaf.is_index_leaf = true;
            size_t end = std::min(i + static_cast<size_t>(mlk), sorted.size());
            for (size_t j = i; j < end; ++j) {
                ileaf.keys.push_back(sorted[j].first);
                ileaf.child_ids.push_back(bytes_to_int(sorted[j].second));
            }
            int id = next_block_id_++;
            oram_data[id] = ileaf.encode();
            level_ids.push_back({ileaf.keys[0], id});
            i = end;
        }
    } else {
        for (size_t i = 0; i < sorted.size(); ) {
            BPlusNode leaf;
            leaf.is_leaf = true;
            size_t end = std::min(i + static_cast<size_t>(mlk), sorted.size());
            for (size_t j = i; j < end; ++j) {
                leaf.keys.push_back(sorted[j].first);
                leaf.values.push_back(sorted[j].second);
            }
            int id = next_block_id_++;
            oram_data[id] = leaf.encode();
            level_ids.push_back({leaf.keys[0], id});
            i = end;
        }
    }

    int max_children = order_;
    while (level_ids.size() > 1) {
        std::vector<std::pair<int, int>> next_level;
        for (size_t i = 0; i < level_ids.size(); ) {
            BPlusNode internal;
            internal.is_leaf = false;
            size_t end = std::min(i + max_children, level_ids.size());
            for (size_t j = i; j < end; ++j) {
                internal.child_ids.push_back(level_ids[j].second);
                internal.child_leaves.push_back(INVALID_LEAF);
                if (j > i)
                    internal.keys.push_back(level_ids[j].first);
            }
            int id = next_block_id_++;
            oram_data[id] = internal.encode();
            next_level.push_back({level_ids[i].first, id});
            i = end;
        }
        level_ids = std::move(next_level);
    }

    return level_ids[0].second;
}

void BPlusOmap::init(const std::vector<std::pair<int, Bytes>>& data) {
    if (data.empty()) {
        root_id_ = INVALID_KEY;
        root_leaf_ = INVALID_LEAF;
        return;
    }

    auto sorted = data;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });

    next_block_id_ = 0;
    std::unordered_map<int, Bytes> oram_data;
    int root = build_tree(sorted, oram_data);

    std::unordered_map<int, BPlusNode> nodes;
    for (auto& [k, v] : oram_data)
        nodes[k] = BPlusNode::decode(v);

    if (split_depth_ > 0) {
        upper_oram_ = PathORAM(std::max(upper_oram_.num_data(), 1),
                               upper_oram_.bucket_size(), 7, storage_creator_);
        oram_ = PathORAM(std::max(next_block_id_, 1), oram_.bucket_size(),
                         7, storage_creator_);

        std::function<void(int, int)> assign_leaves = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (depth < split_depth_)
                upper_oram_.set_leaf(key, upper_oram_.random_leaf());
            else
                oram_.set_leaf(key, oram_.random_leaf());
            if (!nd.is_leaf) {
                for (size_t i = 0; i < nd.child_ids.size(); ++i)
                    assign_leaves(nd.child_ids[i], depth + 1);
            }
        };
        assign_leaves(root, 0);

        std::function<void(int, int)> fix_child_leaves = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (!nd.is_leaf) {
                for (size_t i = 0; i < nd.child_ids.size(); ++i) {
                    PathORAM& co = (depth + 1 < split_depth_) ? upper_oram_ : oram_;
                    nd.child_leaves[i] = co.get_leaf(nd.child_ids[i]);
                }
                for (size_t i = 0; i < nd.child_ids.size(); ++i)
                    fix_child_leaves(nd.child_ids[i], depth + 1);
            }
        };
        fix_child_leaves(root, 0);

        std::unordered_map<int, Bytes> upper_data, lower_data;
        std::function<void(int, int)> partition = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (depth < split_depth_)
                upper_data[key] = nd.encode();
            else
                lower_data[key] = nd.encode();
            if (!nd.is_leaf) {
                for (size_t i = 0; i < nd.child_ids.size(); ++i)
                    partition(nd.child_ids[i], depth + 1);
            }
        };
        partition(root, 0);

        upper_oram_.init(upper_data);
        oram_.init(lower_data);
    } else {
        oram_ = PathORAM(std::max(next_block_id_, 1), oram_.bucket_size(),
                         7, storage_creator_);
        for (auto& [k, _] : nodes)
            oram_.set_leaf(k, oram_.random_leaf());

        for (auto& [k, nd] : nodes) {
            if (!nd.is_leaf) {
                for (size_t i = 0; i < nd.child_ids.size(); ++i)
                    nd.child_leaves[i] = oram_.get_leaf(nd.child_ids[i]);
            }
        }

        oram_data.clear();
        for (auto& [k, nd] : nodes)
            oram_data[k] = nd.encode();

        oram_.init(oram_data);
    }

    root_id_ = root;
    PathORAM& root_oram = oram_for_depth(0);
    root_leaf_ = root_oram.get_leaf(root);
}

// ─── Low-level helpers ──────────────────────────────────────────────────────

int BPlusOmap::find_child_index(const BPlusNode& node, int key) {
    int i = 0;
    while (i < static_cast<int>(node.keys.size()) && key >= node.keys[i])
        ++i;
    return i;
}

int BPlusOmap::find_leaf_index(const BPlusNode& node, int key) {
    for (int i = 0; i < static_cast<int>(node.keys.size()); ++i)
        if (node.keys[i] == key) return i;
    return -1;
}

void BPlusOmap::move_to_local(int id, int leaf, int parent_id, int depth) {
    PathORAM& o = oram_for_depth(depth);
    o.read_path_to_stash(leaf);
    Block block = o.extract_from_stash(id);
    BPlusNode nd = BPlusNode::decode(block.value);
    local_.push_back({id, block.leaf, nd, parent_id, depth});
    o.evict_and_write_path(leaf);
    ++op_count_;
    if (split_depth_ > 0 && depth < split_depth_)
        ++upper_op_count_;
    else
        ++lower_op_count_;
}

void BPlusOmap::move_to_sibling_cache(int id, int leaf,
                                       int parent_local_idx, int child_idx,
                                       int depth) {
    PathORAM& o = oram_for_depth(depth);
    o.read_path_to_stash(leaf);
    Block block = o.extract_from_stash(id);
    BPlusNode nd = BPlusNode::decode(block.value);
    sibling_cache_.push_back({id, block.leaf, nd, parent_local_idx, child_idx, depth});
    o.evict_and_write_path(leaf);
    ++op_count_;
    if (split_depth_ > 0 && depth < split_depth_)
        ++upper_op_count_;
    else
        ++lower_op_count_;
}

int BPlusOmap::split_upper_budget() const {
    if (split_depth_ <= 0) return 0;
    return std::min(2 * split_depth_ - 1, 3 * max_height_);
}

void BPlusOmap::do_dummy_ops(int count) {
    for (int i = 0; i < count; ++i)
        oram_.dummy_access();
    op_count_ += count;
    lower_op_count_ += count;
}

void BPlusOmap::pad_to_budget() {
    int budget = 3 * max_height_;
    if (split_depth_ > 0) {
        int ub = split_upper_budget();
        int upper_pad = std::max(0, ub - upper_op_count_);
        for (int i = 0; i < upper_pad; ++i) upper_oram_.dummy_access();
        upper_op_count_ += upper_pad;

        int lb = budget - ub;
        int lower_pad = std::max(0, lb - lower_op_count_);
        for (int i = 0; i < lower_pad; ++i) oram_.dummy_access();
        lower_op_count_ += lower_pad;

        op_count_ = upper_op_count_ + lower_op_count_;
    } else {
        int pad = std::max(0, budget - op_count_);
        do_dummy_ops(pad);
    }
}

void BPlusOmap::finalize_bw() {
    if (split_depth_ > 0) {
        int ubw = upper_oram_.path_bandwidth_bytes();
        int lbw = oram_.path_bandwidth_bytes();
        last_bw_.rounds = op_count_;
        last_bw_.bytes_downloaded =
            static_cast<uint64_t>(upper_op_count_) * ubw +
            static_cast<uint64_t>(lower_op_count_) * lbw;
    } else {
        int bw = oram_.path_bandwidth_bytes();
        last_bw_.rounds = op_count_;
        last_bw_.bytes_downloaded = static_cast<uint64_t>(op_count_) * bw;
    }
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;
}

int BPlusOmap::min_leaf_keys() const {
    if (index_mode_)
        return std::max(1, order_ * (order_ - 1) / 2);
    return std::max(1, (order_ + 1) / 2 - 1);
}

// ─── Unified traversal: reads target + sibling at every level ───────────────

int BPlusOmap::traverse_with_siblings(int key) {
    local_.clear();
    sibling_cache_.clear();

    int cur_id = root_id_;
    int cur_leaf = root_leaf_;
    int ops_start = op_count_;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_id == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().id;
        move_to_local(cur_id, cur_leaf, parent, d);
        auto& ln = local_.back();

        if (ln.node.is_leaf) {
            oram_for_depth(d).dummy_access();
            op_count_++;
            if (split_depth_ > 0 && d < split_depth_)
                ++upper_op_count_;
            else
                ++lower_op_count_;
            break;
        }

        int ci = find_child_index(ln.node, key);
        int num_ch = static_cast<int>(ln.node.child_ids.size());
        int sib_idx = (ci > 0) ? ci - 1 : ((ci + 1 < num_ch) ? ci + 1 : -1);

        int child_depth = d + 1;
        if (sib_idx >= 0) {
            int ploc = static_cast<int>(local_.size()) - 1;
            move_to_sibling_cache(
                ln.node.child_ids[sib_idx],
                ln.node.child_leaves[sib_idx],
                ploc, sib_idx, child_depth);
        } else {
            oram_for_depth(child_depth).dummy_access();
            op_count_++;
            if (split_depth_ > 0 && child_depth < split_depth_)
                ++upper_op_count_;
            else
                ++lower_op_count_;
        }

        cur_id = ln.node.child_ids[ci];
        cur_leaf = ln.node.child_leaves[ci];
    }

    return op_count_ - ops_start;
}

// ─── Reassign ORAM leaves for all cached nodes ─────────────────────────────

void BPlusOmap::reassign_all_leaves() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& ln = local_[i];
        PathORAM& o = oram_for_depth(ln.depth);
        int nl = o.random_leaf();
        o.set_leaf(ln.id, nl);
        ln.leaf = nl;
    }

    for (auto& sib : sibling_cache_) {
        if (sib.id == INVALID_KEY) continue;
        PathORAM& o = oram_for_depth(sib.depth);
        int nl = o.random_leaf();
        o.set_leaf(sib.id, nl);
        sib.leaf = nl;
    }

    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& child = local_[i];
        if (child.parent_id == INVALID_KEY) continue;
        for (auto& p : local_) {
            if (p.id != child.parent_id || p.node.is_leaf) continue;
            for (size_t j = 0; j < p.node.child_ids.size(); ++j) {
                if (p.node.child_ids[j] == child.id) {
                    p.node.child_leaves[j] = child.leaf;
                    break;
                }
            }
            break;
        }
    }

    for (auto& sib : sibling_cache_) {
        if (sib.id == INVALID_KEY || sib.parent_local_idx < 0) continue;
        if (sib.parent_local_idx >= static_cast<int>(local_.size())) continue;
        auto& p = local_[sib.parent_local_idx];
        if (p.node.is_leaf) continue;
        for (size_t j = 0; j < p.node.child_ids.size(); ++j) {
            if (p.node.child_ids[j] == sib.id) {
                p.node.child_leaves[j] = sib.leaf;
                break;
            }
        }
    }

    for (auto& ln : local_) {
        if (ln.id == root_id_) {
            root_leaf_ = ln.leaf;
            break;
        }
    }
}

// ─── Flush all cached nodes back to ORAM stash ─────────────────────────────

void BPlusOmap::flush_all_to_stash() {
    for (auto& ln : local_) {
        Block block{ln.id, ln.leaf, ln.node.encode()};
        oram_for_depth(ln.depth).add_to_stash(block);
    }
    for (auto& sib : sibling_cache_) {
        if (sib.id == INVALID_KEY) continue;
        Block block{sib.id, sib.leaf, sib.node.encode()};
        oram_for_depth(sib.depth).add_to_stash(block);
    }
    local_.clear();
    sibling_cache_.clear();
}

// ─── Delete underflow handling ──────────────────────────────────────────────

void BPlusOmap::handle_delete_underflow() {
    int mk = min_leaf_keys();

    for (int i = static_cast<int>(local_.size()) - 1; i > 0; --i) {
        auto& nd = local_[i];
        auto& par = local_[i - 1];

        if (static_cast<int>(nd.node.keys.size()) >= mk) continue;

        int nd_idx = -1;
        for (int j = 0; j < static_cast<int>(par.node.child_ids.size()); ++j)
            if (par.node.child_ids[j] == nd.id) { nd_idx = j; break; }
        if (nd_idx < 0) continue;

        CachedSibling* sib = nullptr;
        for (auto& s : sibling_cache_)
            if (s.parent_local_idx == i - 1 && s.id != INVALID_KEY)
                { sib = &s; break; }
        if (!sib) continue;

        bool sib_left = (sib->child_idx_in_parent < nd_idx);
        int sep = sib_left ? sib->child_idx_in_parent : nd_idx;
        if (sep >= static_cast<int>(par.node.keys.size())) continue;

        bool leaf = nd.node.is_leaf;

        if (static_cast<int>(sib->node.keys.size()) > mk) {
            // ─── BORROW ─────────────────────────────────────────────
            if (leaf) {
                bool idx_leaf = nd.node.is_index_leaf;
                if (sib_left) {
                    nd.node.keys.insert(nd.node.keys.begin(), sib->node.keys.back());
                    if (idx_leaf) {
                        nd.node.child_ids.insert(nd.node.child_ids.begin(), sib->node.child_ids.back());
                        sib->node.child_ids.pop_back();
                    } else {
                        nd.node.values.insert(nd.node.values.begin(), sib->node.values.back());
                        sib->node.values.pop_back();
                    }
                    sib->node.keys.pop_back();
                    par.node.keys[sep] = nd.node.keys[0];
                } else {
                    nd.node.keys.push_back(sib->node.keys[0]);
                    if (idx_leaf) {
                        nd.node.child_ids.push_back(sib->node.child_ids[0]);
                        sib->node.child_ids.erase(sib->node.child_ids.begin());
                    } else {
                        nd.node.values.push_back(sib->node.values[0]);
                        sib->node.values.erase(sib->node.values.begin());
                    }
                    sib->node.keys.erase(sib->node.keys.begin());
                    par.node.keys[sep] = sib->node.keys[0];
                }
            } else {
                if (sib_left) {
                    nd.node.keys.insert(nd.node.keys.begin(), par.node.keys[sep]);
                    nd.node.child_ids.insert(nd.node.child_ids.begin(), sib->node.child_ids.back());
                    nd.node.child_leaves.insert(nd.node.child_leaves.begin(), sib->node.child_leaves.back());
                    par.node.keys[sep] = sib->node.keys.back();
                    sib->node.keys.pop_back();
                    sib->node.child_ids.pop_back();
                    sib->node.child_leaves.pop_back();
                } else {
                    nd.node.keys.push_back(par.node.keys[sep]);
                    nd.node.child_ids.push_back(sib->node.child_ids[0]);
                    nd.node.child_leaves.push_back(sib->node.child_leaves[0]);
                    par.node.keys[sep] = sib->node.keys[0];
                    sib->node.keys.erase(sib->node.keys.begin());
                    sib->node.child_ids.erase(sib->node.child_ids.begin());
                    sib->node.child_leaves.erase(sib->node.child_leaves.begin());
                }
            }
        } else {
            // ─── MERGE ──────────────────────────────────────────────
            if (leaf) {
                bool idx_leaf = nd.node.is_index_leaf;
                if (sib_left) {
                    sib->node.keys.insert(sib->node.keys.end(),
                        nd.node.keys.begin(), nd.node.keys.end());
                    if (idx_leaf) {
                        sib->node.child_ids.insert(sib->node.child_ids.end(),
                            nd.node.child_ids.begin(), nd.node.child_ids.end());
                        nd.node.child_ids.clear();
                    } else {
                        sib->node.values.insert(sib->node.values.end(),
                            nd.node.values.begin(), nd.node.values.end());
                        nd.node.values.clear();
                    }
                    par.node.keys.erase(par.node.keys.begin() + sep);
                    par.node.child_ids.erase(par.node.child_ids.begin() + nd_idx);
                    par.node.child_leaves.erase(par.node.child_leaves.begin() + nd_idx);
                    nd.node.keys.clear();
                } else {
                    nd.node.keys.insert(nd.node.keys.end(),
                        sib->node.keys.begin(), sib->node.keys.end());
                    if (idx_leaf) {
                        nd.node.child_ids.insert(nd.node.child_ids.end(),
                            sib->node.child_ids.begin(), sib->node.child_ids.end());
                    } else {
                        nd.node.values.insert(nd.node.values.end(),
                            sib->node.values.begin(), sib->node.values.end());
                    }
                    int sp = -1;
                    for (int j = 0; j < (int)par.node.child_ids.size(); ++j)
                        if (par.node.child_ids[j] == sib->id) { sp = j; break; }
                    if (sp >= 0) {
                        par.node.keys.erase(par.node.keys.begin() + sep);
                        par.node.child_ids.erase(par.node.child_ids.begin() + sp);
                        par.node.child_leaves.erase(par.node.child_leaves.begin() + sp);
                    }
                    sib->id = INVALID_KEY;
                }
            } else {
                if (sib_left) {
                    sib->node.keys.push_back(par.node.keys[sep]);
                    sib->node.keys.insert(sib->node.keys.end(),
                        nd.node.keys.begin(), nd.node.keys.end());
                    sib->node.child_ids.insert(sib->node.child_ids.end(),
                        nd.node.child_ids.begin(), nd.node.child_ids.end());
                    sib->node.child_leaves.insert(sib->node.child_leaves.end(),
                        nd.node.child_leaves.begin(), nd.node.child_leaves.end());
                    par.node.keys.erase(par.node.keys.begin() + sep);
                    par.node.child_ids.erase(par.node.child_ids.begin() + nd_idx);
                    par.node.child_leaves.erase(par.node.child_leaves.begin() + nd_idx);
                    nd.node.keys.clear();
                    nd.node.child_ids.clear();
                    nd.node.child_leaves.clear();
                } else {
                    nd.node.keys.push_back(par.node.keys[sep]);
                    nd.node.keys.insert(nd.node.keys.end(),
                        sib->node.keys.begin(), sib->node.keys.end());
                    nd.node.child_ids.insert(nd.node.child_ids.end(),
                        sib->node.child_ids.begin(), sib->node.child_ids.end());
                    nd.node.child_leaves.insert(nd.node.child_leaves.end(),
                        sib->node.child_leaves.begin(), sib->node.child_leaves.end());
                    int sp = -1;
                    for (int j = 0; j < (int)par.node.child_ids.size(); ++j)
                        if (par.node.child_ids[j] == sib->id) { sp = j; break; }
                    if (sp >= 0) {
                        par.node.keys.erase(par.node.keys.begin() + sep);
                        par.node.child_ids.erase(par.node.child_ids.begin() + sp);
                        par.node.child_leaves.erase(par.node.child_leaves.begin() + sp);
                    }
                    sib->id = INVALID_KEY;
                }
            }
        }
    }

    if (!local_.empty() && !local_[0].node.is_leaf && local_[0].node.keys.empty()) {
        if (local_[0].node.child_ids.size() == 1) {
            root_id_ = local_[0].node.child_ids[0];
        } else if (local_[0].node.child_ids.empty()) {
            root_id_ = INVALID_KEY;
            root_leaf_ = INVALID_LEAF;
        }
    }
}

// ─── Split helpers (unchanged) ──────────────────────────────────────────────

int BPlusOmap::split_leaf(LocalNode& leaf_ln) {
    auto& nd = leaf_ln.node;
    int mid = static_cast<int>(nd.keys.size()) / 2;

    BPlusNode new_leaf;
    new_leaf.is_leaf = true;
    new_leaf.is_index_leaf = nd.is_index_leaf;
    new_leaf.keys.assign(nd.keys.begin() + mid, nd.keys.end());
    if (nd.is_index_leaf) {
        new_leaf.child_ids.assign(nd.child_ids.begin() + mid, nd.child_ids.end());
        nd.child_ids.resize(mid);
    } else {
        new_leaf.values.assign(nd.values.begin() + mid, nd.values.end());
        nd.values.resize(mid);
    }
    nd.keys.resize(mid);

    int new_id = next_block_id_++;
    PathORAM& o = oram_for_depth(leaf_ln.depth);
    int new_lf = o.random_leaf();
    o.set_leaf(new_id, new_lf);
    local_.push_back({new_id, new_lf, new_leaf, leaf_ln.parent_id, leaf_ln.depth});
    return new_id;
}

int BPlusOmap::split_internal(LocalNode& int_ln) {
    auto& nd = int_ln.node;
    int mid = static_cast<int>(nd.keys.size()) / 2;

    BPlusNode new_internal;
    new_internal.is_leaf = false;
    new_internal.keys.assign(nd.keys.begin() + mid + 1, nd.keys.end());
    new_internal.child_ids.assign(nd.child_ids.begin() + mid + 1, nd.child_ids.end());
    new_internal.child_leaves.assign(nd.child_leaves.begin() + mid + 1, nd.child_leaves.end());

    nd.keys.resize(mid);
    nd.child_ids.resize(mid + 1);
    nd.child_leaves.resize(mid + 1);

    int new_id = next_block_id_++;
    PathORAM& o = oram_for_depth(int_ln.depth);
    int new_lf = o.random_leaf();
    o.set_leaf(new_id, new_lf);
    local_.push_back({new_id, new_lf, new_internal, int_ln.parent_id, int_ln.depth});
    return new_id;
}

// ═══════════════════════════════════════════════════════════════════════════
// All three operations use traverse_with_siblings + pad to 2*max_height_
// so the server sees an identical ORAM access pattern regardless of op type.
// ═══════════════════════════════════════════════════════════════════════════

Bytes BPlusOmap::search(int key, const Bytes* update) {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        if (!ods_mode_) pad_to_budget();
        finalize_bw();
        return {};
    }

    traverse_with_siblings(key);

    Bytes result;
    if (!local_.empty() && local_.back().node.is_leaf) {
        if (local_.back().node.is_index_leaf) {
            int idx = find_leaf_index(local_.back().node, key);
            if (idx >= 0)
                result = int_to_bytes(local_.back().node.child_ids[idx]);
        } else {
            int idx = find_leaf_index(local_.back().node, key);
            if (idx >= 0) {
                result = local_.back().node.values[idx];
                if (update) local_.back().node.values[idx] = *update;
            }
        }
    }

    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) pad_to_budget();
    finalize_bw();
    return result;
}

void BPlusOmap::insert(int key, const Bytes& value) {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        if (index_mode_) {
            BPlusNode ileaf;
            ileaf.is_leaf = true;
            ileaf.is_index_leaf = true;
            ileaf.keys = {key};
            ileaf.child_ids = {bytes_to_int(value)};
            int id = next_block_id_++;
            PathORAM& root_o = oram_for_depth(0);
            int lf = root_o.random_leaf();
            root_o.set_leaf(id, lf);
            root_o.add_to_stash({id, lf, ileaf.encode()});
            root_id_ = id;
            root_leaf_ = lf;
        } else {
            BPlusNode leaf;
            leaf.is_leaf = true;
            leaf.keys = {key};
            leaf.values = {value};
            int id = next_block_id_++;
            PathORAM& root_o = oram_for_depth(0);
            int lf = root_o.random_leaf();
            root_o.set_leaf(id, lf);
            root_o.add_to_stash({id, lf, leaf.encode()});
            root_id_ = id;
            root_leaf_ = lf;
        }
        if (!ods_mode_) pad_to_budget();
        finalize_bw();
        return;
    }

    traverse_with_siblings(key);

    if (!local_.empty() && local_.back().node.is_leaf) {
        int leaf_idx = static_cast<int>(local_.size()) - 1;
        {
            auto& leaf_ln = local_[leaf_idx];
            auto it = std::lower_bound(leaf_ln.node.keys.begin(),
                                       leaf_ln.node.keys.end(), key);
            int pos = static_cast<int>(it - leaf_ln.node.keys.begin());
            leaf_ln.node.keys.insert(it, key);
            if (leaf_ln.node.is_index_leaf) {
                leaf_ln.node.child_ids.insert(
                    leaf_ln.node.child_ids.begin() + pos, bytes_to_int(value));
            } else {
                leaf_ln.node.values.insert(
                    leaf_ln.node.values.begin() + pos, value);
            }
        }

        if (static_cast<int>(local_[leaf_idx].node.keys.size()) > max_leaf_keys()) {
            int new_id = split_leaf(local_[leaf_idx]);
            int parent_id = local_[leaf_idx].parent_id;
            int new_depth = local_[leaf_idx].depth;
            PathORAM& new_o = oram_for_depth(new_depth);
            if (parent_id != INVALID_KEY) {
                for (auto& p : local_) {
                    if (p.id == parent_id && !p.node.is_leaf) {
                        int ci = find_child_index(
                            p.node, local_[leaf_idx].node.keys.back());
                        int sep_key = local_.back().node.keys[0];
                        p.node.keys.insert(p.node.keys.begin() + ci, sep_key);
                        int new_leaf_val = new_o.get_leaf(new_id);
                        p.node.child_ids.insert(
                            p.node.child_ids.begin() + ci + 1, new_id);
                        p.node.child_leaves.insert(
                            p.node.child_leaves.begin() + ci + 1, new_leaf_val);
                        break;
                    }
                }
            } else {
                int sep_key = local_.back().node.keys[0];
                int old_id = local_[leaf_idx].id;
                int old_leaf = local_[leaf_idx].leaf;
                BPlusNode new_root;
                new_root.is_leaf = false;
                new_root.keys = {sep_key};
                new_root.child_ids = {old_id, new_id};
                new_root.child_leaves = {old_leaf, new_o.get_leaf(new_id)};
                int rid = next_block_id_++;
                PathORAM& root_o = oram_for_depth(0);
                int rlf = root_o.random_leaf();
                root_o.set_leaf(rid, rlf);
                local_.push_back({rid, rlf, new_root, INVALID_KEY, 0});
                root_id_ = rid;
            }
        }
    }

    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) pad_to_budget();
    finalize_bw();
}

void BPlusOmap::remove(int key) {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        if (!ods_mode_) pad_to_budget();
        finalize_bw();
        return;
    }

    traverse_with_siblings(key);

    if (!local_.empty() && local_.back().node.is_leaf) {
        int idx = find_leaf_index(local_.back().node, key);
        if (idx >= 0) {
            local_.back().node.keys.erase(local_.back().node.keys.begin() + idx);
            if (local_.back().node.is_index_leaf)
                local_.back().node.child_ids.erase(local_.back().node.child_ids.begin() + idx);
            else
                local_.back().node.values.erase(local_.back().node.values.begin() + idx);
        }
    }

    handle_delete_underflow();
    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) pad_to_budget();
    finalize_bw();
}

void BPlusOmap::dummy_access() {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;
    if (!ods_mode_) pad_to_budget();
    finalize_bw();
}

void BPlusOmap::partial_dummy_access() {
    if (split_depth_ <= 0) {
        dummy_access();
        return;
    }
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;

    int eff_split = std::min(split_depth_, 3 * max_height_);
    for (int i = 0; i < eff_split; ++i)
        upper_oram_.dummy_access();
    upper_op_count_ = eff_split;
    op_count_ += eff_split;

    oram_.dummy_access();
    lower_op_count_ = 1;
    op_count_++;

    finalize_bw();
}

// ─── Step-by-step interface for interleaved access ─────────────────────────

void BPlusOmap::begin_step_search(int key, const Bytes* update) {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;
    local_.clear();
    sibling_cache_.clear();
    pb_ = PBState{};

    ss_ = StepState{};
    ss_.key = key;
    ss_.update = update;
    ss_.budget = 3 * max_height_;

    if (root_id_ == INVALID_KEY) {
        ss_.phase = StepPhase::PAD;
        ss_.pad_remaining = ss_.budget;
    } else {
        ss_.phase = StepPhase::TRAVERSE_TARGET;
        ss_.cur_id = root_id_;
        ss_.cur_leaf = root_leaf_;
        ss_.depth = 0;
    }
}

void BPlusOmap::begin_step_dummy() {
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;
    local_.clear();
    sibling_cache_.clear();
    pb_ = PBState{};

    ss_ = StepState{};
    ss_.is_dummy = true;
    ss_.budget = 3 * max_height_;
    ss_.phase = StepPhase::PAD;
    ss_.pad_remaining = ss_.budget;
    if (split_depth_ > 0)
        ss_.partial_upper_pad = split_upper_budget();
}

void BPlusOmap::begin_step_partial_dummy() {
    if (split_depth_ <= 0) {
        begin_step_dummy();
        return;
    }
    last_bw_.reset();
    op_count_ = 0; upper_op_count_ = 0; lower_op_count_ = 0;
    local_.clear();
    sibling_cache_.clear();
    pb_ = PBState{};

    int eff_split = std::min(split_depth_, 3 * max_height_);
    ss_ = StepState{};
    ss_.is_dummy = true;
    ss_.budget = eff_split + 1;
    ss_.phase = StepPhase::PAD;
    ss_.pad_remaining = eff_split + 1;
    ss_.partial_upper_pad = eff_split;
}

OramStepRound BPlusOmap::step_next_round() {
    OramStepRound r;
    ss_.round_read = (ss_.phase != StepPhase::DONE && ss_.phase != StepPhase::DECISION);
    pb_.round_read = (pb_.active && pb_.phase != StepPhase::DONE
                      && pb_.phase != StepPhase::DECISION);

    // ── Main operation ──
    if (ss_.round_read) {
        int depth = ss_.depth;
        int leaf = INVALID_LEAF;
        if (ss_.phase == StepPhase::TRAVERSE_TARGET) {
            leaf = ss_.cur_leaf;
            ss_.cur_round_depth = depth;
        } else if (ss_.phase == StepPhase::TRAVERSE_SIBLING) {
            int sib_depth = depth + 1;
            PathORAM& so = oram_for_depth(sib_depth);
            leaf = ss_.sibling_is_dummy ? so.random_leaf() : ss_.sib_leaf;
            ss_.cur_round_depth = sib_depth;
        } else {
            if (ss_.partial_upper_pad > 0) {
                leaf = upper_oram_.random_leaf();
                ss_.cur_round_depth = 0;
            } else {
                leaf = oram_.random_leaf();
                ss_.cur_round_depth = split_depth_;
            }
        }
        ss_.cur_round_leaf = leaf;
        PathORAM& ro = oram_for_depth(ss_.cur_round_depth);
        r.reads.push_back({ro.get_store_id(), leaf});
    }

    // ── Piggyback operation ──
    if (pb_.round_read) {
        int leaf = INVALID_LEAF;
        if (pb_.phase == StepPhase::TRAVERSE_TARGET) {
            pb_.node_in_local = false;
            for (auto& ln : local_)
                if (ln.id == pb_.cur_id) { pb_.node_in_local = true; break; }
            PathORAM& po = oram_for_depth(pb_.depth);
            leaf = pb_.node_in_local ? po.random_leaf() : pb_.cur_leaf;
            pb_.cur_round_depth = pb_.depth;
        } else if (pb_.phase == StepPhase::TRAVERSE_SIBLING) {
            int sib_depth = pb_.depth + 1;
            PathORAM& so = oram_for_depth(sib_depth);
            leaf = pb_.sibling_is_dummy ? so.random_leaf() : pb_.sib_leaf;
            pb_.cur_round_depth = sib_depth;
        } else {
            if (pb_.partial_upper_pad > 0) {
                leaf = upper_oram_.random_leaf();
                pb_.cur_round_depth = 0;
            } else {
                leaf = oram_.random_leaf();
                pb_.cur_round_depth = split_depth_;
            }
        }
        pb_.cur_round_leaf = leaf;
        PathORAM& po = oram_for_depth(pb_.cur_round_depth);
        r.reads.push_back({po.get_store_id(), leaf});
    }

    return r;
}

void BPlusOmap::step_apply_reads(const std::vector<PathData>& results) {
    if (ss_.round_read && pb_.round_read && results.size() >= 2) {
        PathORAM& main_oram = oram_for_depth(ss_.cur_round_depth);
        PathORAM& pb_oram = oram_for_depth(pb_.cur_round_depth);
        if (&main_oram == &pb_oram) {
            PathData merged(results[0]);
            for (auto& [nid, blocks] : results[1])
                if (merged.find(nid) == merged.end())
                    merged[nid] = blocks;
            main_oram.apply_fetched_path(std::move(merged));
        } else {
            main_oram.apply_fetched_path(PathData(results[0]));
            pb_oram.apply_fetched_path(PathData(results[1]));
        }
    } else if (ss_.round_read && !results.empty()) {
        oram_for_depth(ss_.cur_round_depth)
            .apply_fetched_path(PathData(results[0]));
    } else if (pb_.round_read && !results.empty()) {
        oram_for_depth(pb_.cur_round_depth)
            .apply_fetched_path(PathData(results[0]));
    }
}

void BPlusOmap::step_process() {
    if (ss_.phase == StepPhase::TRAVERSE_TARGET) {
        PathORAM& to = oram_for_depth(ss_.depth);
        Block block = to.extract_from_stash(ss_.cur_id);
        BPlusNode nd = BPlusNode::decode(block.value);
        int parent = local_.empty() ? INVALID_KEY : local_.back().id;
        local_.push_back({ss_.cur_id, block.leaf, nd, parent, ss_.depth});
        ss_.ops++;
        ++op_count_;
        if (split_depth_ > 0 && ss_.depth < split_depth_)
            ++upper_op_count_;
        else
            ++lower_op_count_;

        auto& ln = local_.back();
        if (ln.node.is_leaf) {
            int idx = find_leaf_index(ln.node, ss_.key);
            if (idx >= 0) {
                if (ln.node.is_index_leaf) {
                    ss_.result = int_to_bytes(ln.node.child_ids[idx]);
                } else {
                    ss_.result = ln.node.values[idx];
                    if (ss_.update) ln.node.values[idx] = *ss_.update;
                }
            }
            ss_.leaf_reached = true;
            ss_.sibling_is_dummy = true;
            ss_.phase = StepPhase::TRAVERSE_SIBLING;
        } else {
            int ci = find_child_index(ln.node, ss_.key);
            int num_ch = static_cast<int>(ln.node.child_ids.size());
            int sib_idx = (ci > 0) ? ci - 1
                                   : ((ci + 1 < num_ch) ? ci + 1 : -1);
            ss_.next_id = ln.node.child_ids[ci];
            ss_.next_leaf = ln.node.child_leaves[ci];

            if (sib_idx >= 0) {
                ss_.sibling_is_dummy = false;
                ss_.sib_id = ln.node.child_ids[sib_idx];
                ss_.sib_leaf = ln.node.child_leaves[sib_idx];
                ss_.sib_parent_idx = static_cast<int>(local_.size()) - 1;
                ss_.sib_child_idx = sib_idx;
            } else {
                ss_.sibling_is_dummy = true;
            }
            ss_.phase = StepPhase::TRAVERSE_SIBLING;
        }

    } else if (ss_.phase == StepPhase::TRAVERSE_SIBLING) {
        int sib_depth = ss_.depth + 1;
        if (!ss_.sibling_is_dummy) {
            PathORAM& so = oram_for_depth(sib_depth);
            Block block = so.extract_from_stash(ss_.sib_id);
            BPlusNode nd = BPlusNode::decode(block.value);
            sibling_cache_.push_back({ss_.sib_id, block.leaf, nd,
                                      ss_.sib_parent_idx, ss_.sib_child_idx,
                                      sib_depth});
        }
        ss_.ops++;
        ++op_count_;
        if (split_depth_ > 0 && sib_depth < split_depth_)
            ++upper_op_count_;
        else
            ++lower_op_count_;

        if (ss_.leaf_reached || ss_.next_id == INVALID_KEY) {
            if (ss_.decision_enabled && !ss_.result.empty()) {
                ss_.phase = StepPhase::DECISION;
            } else {
                reassign_all_leaves();
                flush_all_to_stash();
                ss_.pad_remaining = std::max(0, ss_.budget - ss_.ops);
                if (split_depth_ > 0)
                    ss_.partial_upper_pad = std::max(0,
                        split_upper_budget() - upper_op_count_);
                ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::PAD
                                                    : StepPhase::DONE;
            }
        } else {
            ss_.cur_id = ss_.next_id;
            ss_.cur_leaf = ss_.next_leaf;
            ss_.depth++;
            ss_.phase = StepPhase::TRAVERSE_TARGET;
        }

    } else if (ss_.phase == StepPhase::PAD) {
        ss_.ops++;
        ++op_count_;
        if (ss_.partial_upper_pad > 0) {
            ss_.partial_upper_pad--;
            ++upper_op_count_;
        } else {
            ++lower_op_count_;
        }
        ss_.pad_remaining--;
        if (ss_.pad_remaining <= 0)
            ss_.phase = StepPhase::DONE;
    }

    // ── Piggyback operation ──
    if (!pb_.active || pb_.phase == StepPhase::DONE)
        return;

    if (pb_.phase == StepPhase::TRAVERSE_TARGET) {
        int local_idx = -1;
        if (pb_.node_in_local) {
            for (int i = 0; i < static_cast<int>(local_.size()); ++i)
                if (local_[i].id == pb_.cur_id) { local_idx = i; break; }
            if (local_idx < 0) {
                // Main flushed local_; re-extract from stash
                PathORAM& po = oram_for_depth(pb_.depth);
                Block block = po.extract_from_stash(pb_.cur_id);
                BPlusNode nd = BPlusNode::decode(block.value);
                int parent = INVALID_KEY;
                for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
                    for (int c = 0; c < static_cast<int>(local_[i].node.child_ids.size()); ++c)
                        if (local_[i].node.child_ids[c] == pb_.cur_id) { parent = local_[i].id; break; }
                    if (parent != INVALID_KEY) break;
                }
                local_.push_back({pb_.cur_id, block.leaf, nd, parent, pb_.depth});
                local_idx = static_cast<int>(local_.size()) - 1;
            }
        } else {
            // Main op may have added this node to local_ earlier in the same round
            for (int i = 0; i < static_cast<int>(local_.size()); ++i)
                if (local_[i].id == pb_.cur_id) { local_idx = i; break; }
            if (local_idx < 0) {
                PathORAM& po = oram_for_depth(pb_.depth);
                Block block = po.extract_from_stash(pb_.cur_id);
                BPlusNode nd = BPlusNode::decode(block.value);
                int parent = INVALID_KEY;
                for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
                    for (int c = 0; c < static_cast<int>(local_[i].node.child_ids.size()); ++c)
                        if (local_[i].node.child_ids[c] == pb_.cur_id) { parent = local_[i].id; break; }
                    if (parent != INVALID_KEY) break;
                }
                local_.push_back({pb_.cur_id, block.leaf, nd, parent, pb_.depth});
                local_idx = static_cast<int>(local_.size()) - 1;
            }
        }
        pb_.ops++;
        ++op_count_;
        if (split_depth_ > 0 && pb_.depth < split_depth_)
            ++upper_op_count_;
        else
            ++lower_op_count_;

        if (local_idx >= 0) {
            auto& ln = local_[local_idx];
            if (ln.node.is_leaf) {
                if (pb_.is_insert) {
                    auto it = std::lower_bound(ln.node.keys.begin(),
                                               ln.node.keys.end(), pb_.key);
                    int pos = static_cast<int>(it - ln.node.keys.begin());
                    ln.node.keys.insert(it, pb_.key);
                    if (ln.node.is_index_leaf) {
                        ln.node.child_ids.insert(
                            ln.node.child_ids.begin() + pos,
                            bytes_to_int(pb_.insert_value));
                    } else {
                        ln.node.values.insert(
                            ln.node.values.begin() + pos, pb_.insert_value);
                    }
                    if (static_cast<int>(ln.node.keys.size()) > max_leaf_keys()) {
                        int new_id = split_leaf(local_[local_idx]);
                        int parent_id = local_[local_idx].parent_id;
                        int new_depth = local_[local_idx].depth;
                        PathORAM& new_o = oram_for_depth(new_depth);
                        if (parent_id != INVALID_KEY) {
                            for (auto& p : local_) {
                                if (p.id == parent_id && !p.node.is_leaf) {
                                    int ci2 = find_child_index(
                                        p.node, local_[local_idx].node.keys.back());
                                    int sep_key = local_.back().node.keys[0];
                                    p.node.keys.insert(
                                        p.node.keys.begin() + ci2, sep_key);
                                    int nlf = new_o.get_leaf(new_id);
                                    p.node.child_ids.insert(
                                        p.node.child_ids.begin() + ci2 + 1, new_id);
                                    p.node.child_leaves.insert(
                                        p.node.child_leaves.begin() + ci2 + 1, nlf);
                                    break;
                                }
                            }
                        } else {
                            int sep_key = local_.back().node.keys[0];
                            int old_id = local_[local_idx].id;
                            int old_leaf = local_[local_idx].leaf;
                            BPlusNode new_root;
                            new_root.is_leaf = false;
                            new_root.keys = {sep_key};
                            new_root.child_ids = {old_id, new_id};
                            new_root.child_leaves = {old_leaf, new_o.get_leaf(new_id)};
                            int rid = next_block_id_++;
                            PathORAM& root_o = oram_for_depth(0);
                            int rlf = root_o.random_leaf();
                            root_o.set_leaf(rid, rlf);
                            local_.push_back({rid, rlf, new_root, INVALID_KEY, 0});
                            root_id_ = rid;
                        }
                    }
                } else {
                    int idx = find_leaf_index(ln.node, pb_.key);
                    if (idx >= 0) {
                        if (ln.node.is_index_leaf)
                            pb_.result = int_to_bytes(ln.node.child_ids[idx]);
                        else
                            pb_.result = ln.node.values[idx];
                    }
                }
                pb_.leaf_reached = true;
                pb_.sibling_is_dummy = true;
                pb_.phase = StepPhase::TRAVERSE_SIBLING;
            } else {
                int ci = find_child_index(ln.node, pb_.key);
                int num_ch = static_cast<int>(ln.node.child_ids.size());
                int sib_idx = (ci > 0) ? ci - 1
                                       : ((ci + 1 < num_ch) ? ci + 1 : -1);
                pb_.next_id = ln.node.child_ids[ci];
                pb_.next_leaf = ln.node.child_leaves[ci];
                if (pb_.node_in_local && local_idx >= 0) {
                    pb_.sibling_is_dummy = true;
                } else if (sib_idx >= 0) {
                    pb_.sibling_is_dummy = false;
                    pb_.sib_id = ln.node.child_ids[sib_idx];
                    pb_.sib_leaf = ln.node.child_leaves[sib_idx];
                    pb_.sib_parent_idx = local_idx;
                    pb_.sib_child_idx = sib_idx;
                } else {
                    pb_.sibling_is_dummy = true;
                }
                pb_.phase = StepPhase::TRAVERSE_SIBLING;
            }
        }
    } else if (pb_.phase == StepPhase::TRAVERSE_SIBLING) {
        int sib_depth = pb_.depth + 1;
        if (!pb_.sibling_is_dummy) {
            PathORAM& so = oram_for_depth(sib_depth);
            Block block = so.extract_from_stash(pb_.sib_id);
            BPlusNode nd = BPlusNode::decode(block.value);
            sibling_cache_.push_back({pb_.sib_id, block.leaf, nd,
                                      pb_.sib_parent_idx, pb_.sib_child_idx,
                                      sib_depth});
        }
        pb_.ops++;
        {
            int pb_sib_depth = pb_.depth + 1;
            ++op_count_;
            if (split_depth_ > 0 && pb_sib_depth < split_depth_)
                ++upper_op_count_;
            else
                ++lower_op_count_;
        }

        if (pb_.leaf_reached || pb_.next_id == INVALID_KEY) {
            pb_.traverse_done = true;
            if (pb_.decision_enabled && !pb_.result.empty()) {
                pb_.phase = StepPhase::DECISION;
            } else {
                pb_.pad_remaining = std::max(0, pb_.budget - pb_.ops);
                if (split_depth_ > 0)
                    pb_.partial_upper_pad = pb_.pad_remaining;
                pb_.phase = (pb_.pad_remaining > 0) ? StepPhase::PAD
                                                    : StepPhase::DONE;
            }
        } else {
            pb_.cur_id = pb_.next_id;
            pb_.cur_leaf = pb_.next_leaf;
            pb_.depth++;
            pb_.phase = StepPhase::TRAVERSE_TARGET;
        }
    } else if (pb_.phase == StepPhase::PAD) {
        pb_.ops++;
        ++op_count_;
        if (pb_.partial_upper_pad > 0) {
            pb_.partial_upper_pad--;
            ++upper_op_count_;
        } else {
            ++lower_op_count_;
        }
        pb_.pad_remaining--;
        if (pb_.pad_remaining <= 0) pb_.phase = StepPhase::DONE;
    }
}

std::vector<StepWriteReq> BPlusOmap::step_prepare_writes() {
    std::vector<StepWriteReq> writes;

    if (!ss_.round_read && !pb_.round_read)
        return writes;

    if (ss_.round_read && pb_.round_read) {
        PathORAM& main_oram = oram_for_depth(ss_.cur_round_depth);
        PathORAM& pb_oram = oram_for_depth(pb_.cur_round_depth);
        if (&main_oram == &pb_oram) {
            writes.push_back({main_oram.get_store_id(),
                              main_oram.prepare_eviction_paths(
                                  {ss_.cur_round_leaf, pb_.cur_round_leaf})});
        } else {
            writes.push_back({main_oram.get_store_id(),
                              main_oram.prepare_eviction(ss_.cur_round_leaf)});
            writes.push_back({pb_oram.get_store_id(),
                              pb_oram.prepare_eviction(pb_.cur_round_leaf)});
        }
    } else if (ss_.round_read) {
        PathORAM& ro = oram_for_depth(ss_.cur_round_depth);
        writes.push_back({ro.get_store_id(),
                          ro.prepare_eviction(ss_.cur_round_leaf)});
    } else {
        PathORAM& po = oram_for_depth(pb_.cur_round_depth);
        writes.push_back({po.get_store_id(),
                          po.prepare_eviction(pb_.cur_round_leaf)});
    }
    return writes;
}

bool BPlusOmap::step_done() const {
    if (pb_.active && pb_.phase != StepPhase::DONE)
        return false;
    return ss_.phase == StepPhase::DONE;
}

Bytes BPlusOmap::step_finish() {
    finalize_bw();
    return ss_.result;
}

void BPlusOmap::step_abort() {
    finalize_bw();
    ss_.phase = StepPhase::DONE;
    pb_ = PBState{};
}

// ─── Piggyback step interface ───────────────────────────────────────────────

void BPlusOmap::begin_piggyback_search(int key) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.key = key;
    pb_.budget = 3 * max_height_;
    if (root_id_ == INVALID_KEY) {
        pb_.phase = StepPhase::PAD;
        pb_.pad_remaining = pb_.budget;
        pb_.traverse_done = true;
        if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
    } else {
        pb_.phase = StepPhase::TRAVERSE_TARGET;
        pb_.cur_id = root_id_;
        pb_.cur_leaf = root_leaf_;
        pb_.depth = 0;
    }
}

void BPlusOmap::begin_piggyback_insert(int key, const Bytes& value) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.key = key;
    pb_.is_insert = true;
    pb_.insert_value = value;
    pb_.budget = 3 * max_height_;
    if (root_id_ == INVALID_KEY) {
        BPlusNode leaf;
        leaf.is_leaf = true;
        if (index_mode_) {
            leaf.is_index_leaf = true;
            leaf.keys = {key};
            leaf.child_ids = {bytes_to_int(value)};
        } else {
            leaf.keys = {key};
            leaf.values = {value};
        }
        int id = next_block_id_++;
        PathORAM& ro = oram_for_depth(0);
        int lf = ro.random_leaf();
        ro.set_leaf(id, lf);
        local_.push_back({id, lf, leaf, INVALID_KEY, 0});
        root_id_ = id;
        root_leaf_ = lf;
        pb_.phase = StepPhase::PAD;
        pb_.pad_remaining = pb_.budget;
        pb_.traverse_done = true;
        if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
    } else {
        pb_.phase = StepPhase::TRAVERSE_TARGET;
        pb_.cur_id = root_id_;
        pb_.cur_leaf = root_leaf_;
        pb_.depth = 0;
    }
}

void BPlusOmap::begin_piggyback_dummy() {
    pb_ = PBState{};
    pb_.active = true;
    pb_.phase = StepPhase::PAD;
    pb_.budget = 3 * max_height_;
    pb_.pad_remaining = pb_.budget;
    pb_.traverse_done = true;
    if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
}

Bytes BPlusOmap::finish_piggyback() { return pb_.result; }

bool BPlusOmap::piggyback_needs_decision() const {
    return pb_.active && pb_.phase == StepPhase::DECISION;
}

Bytes BPlusOmap::piggyback_get_traverse_result() { return pb_.result; }

// ─── Mid-access decision interface ─────────────────────────────────────────

void BPlusOmap::set_step_decision_enabled(bool enable) {
    ss_.decision_enabled = enable;
}

bool BPlusOmap::step_needs_decision() const {
    return ss_.phase == StepPhase::DECISION;
}

Bytes BPlusOmap::step_get_traverse_result() {
    return ss_.result;
}

void BPlusOmap::step_commit_remove() {
    if (ss_.phase != StepPhase::DECISION) return;
    if (!local_.empty() && local_.back().node.is_leaf) {
        int idx = find_leaf_index(local_.back().node, ss_.key);
        if (idx >= 0) {
            local_.back().node.keys.erase(local_.back().node.keys.begin() + idx);
            if (local_.back().node.is_index_leaf)
                local_.back().node.child_ids.erase(local_.back().node.child_ids.begin() + idx);
            else
                local_.back().node.values.erase(local_.back().node.values.begin() + idx);
        }
        handle_delete_underflow();
    }
    reassign_all_leaves();
    flush_all_to_stash();
    ss_.pad_remaining = std::max(0, ss_.budget - ss_.ops);
    if (split_depth_ > 0)
        ss_.partial_upper_pad = std::max(0,
            split_upper_budget() - upper_op_count_);
    ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::PAD : StepPhase::DONE;
}

void BPlusOmap::step_commit_noop() {
    if (ss_.phase != StepPhase::DECISION) return;
    reassign_all_leaves();
    flush_all_to_stash();
    ss_.pad_remaining = std::max(0, ss_.budget - ss_.ops);
    if (split_depth_ > 0)
        ss_.partial_upper_pad = std::max(0,
            split_upper_budget() - upper_op_count_);
    ss_.phase = (ss_.pad_remaining > 0) ? StepPhase::PAD : StepPhase::DONE;
}

void BPlusOmap::set_piggyback_decision_enabled(bool enable) {
    pb_.decision_enabled = enable;
}

void BPlusOmap::piggyback_commit_remove() {
    if (pb_.phase != StepPhase::DECISION) return;
    if (!local_.empty()) {
        int leaf_idx = -1;
        for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
            if (local_[i].node.is_leaf) { leaf_idx = i; break; }
        }
        if (leaf_idx >= 0) {
            int idx = find_leaf_index(local_[leaf_idx].node, pb_.key);
            if (idx >= 0) {
                local_[leaf_idx].node.keys.erase(
                    local_[leaf_idx].node.keys.begin() + idx);
                if (local_[leaf_idx].node.is_index_leaf)
                    local_[leaf_idx].node.child_ids.erase(
                        local_[leaf_idx].node.child_ids.begin() + idx);
                else
                    local_[leaf_idx].node.values.erase(
                        local_[leaf_idx].node.values.begin() + idx);
            }
            handle_delete_underflow();
        }
    }
    reassign_all_leaves();
    flush_all_to_stash();
    pb_.pad_remaining = std::max(0, pb_.budget - pb_.ops);
    if (split_depth_ > 0)
        pb_.partial_upper_pad = pb_.pad_remaining;
    pb_.phase = (pb_.pad_remaining > 0) ? StepPhase::PAD : StepPhase::DONE;
}

void BPlusOmap::piggyback_commit_noop() {
    if (pb_.phase != StepPhase::DECISION) return;
    reassign_all_leaves();
    flush_all_to_stash();
    pb_.pad_remaining = std::max(0, pb_.budget - pb_.ops);
    if (split_depth_ > 0)
        pb_.partial_upper_pad = pb_.pad_remaining;
    pb_.phase = (pb_.pad_remaining > 0) ? StepPhase::PAD : StepPhase::DONE;
}

Bytes BPlusOmap::search_piggyback(int key, const Bytes* update,
                                   int extra_key, char extra_op,
                                   const Bytes* extra_value,
                                   Bytes* extra_result) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        do_dummy_ops(3 * max_height_);
        finalize_bw();
        return {};
    }

    // Phase 1: traverse for the main key (h rounds + h sibling rounds).
    traverse_with_siblings(key);

    // Process main search result.
    Bytes result;
    if (!local_.empty() && local_.back().node.is_leaf) {
        int idx = find_leaf_index(local_.back().node, key);
        if (idx >= 0) {
            if (local_.back().node.is_index_leaf) {
                result = int_to_bytes(local_.back().node.child_ids[idx]);
            } else {
                result = local_.back().node.values[idx];
                if (update) local_.back().node.values[idx] = *update;
            }
        }
    }

    // Flush main traversal to stash so the ORAM is ready for phase 2.
    reassign_all_leaves();
    flush_all_to_stash();
    int main_ops = op_count_;

    // Phase 2: piggyback — traverse for extra_key within remaining budget.
    int budget = 3 * max_height_ - main_ops;
    if (extra_key != INVALID_KEY && budget > 0) {
        traverse_with_siblings(extra_key);
        int extra_ops_used = op_count_ - main_ops;

        if (extra_op == 's' && extra_result) {
            if (!local_.empty() && local_.back().node.is_leaf) {
                int idx = find_leaf_index(local_.back().node, extra_key);
                if (idx >= 0) {
                    if (local_.back().node.is_index_leaf)
                        *extra_result = int_to_bytes(local_.back().node.child_ids[idx]);
                    else
                        *extra_result = local_.back().node.values[idx];
                }
            }
        } else if (extra_op == 'd') {
            if (!local_.empty() && local_.back().node.is_leaf) {
                int idx = find_leaf_index(local_.back().node, extra_key);
                if (idx >= 0) {
                    if (local_.back().node.is_index_leaf) {
                        if (extra_result)
                            *extra_result = int_to_bytes(local_.back().node.child_ids[idx]);
                        local_.back().node.keys.erase(
                            local_.back().node.keys.begin() + idx);
                        local_.back().node.child_ids.erase(
                            local_.back().node.child_ids.begin() + idx);
                    } else {
                        if (extra_result)
                            *extra_result = local_.back().node.values[idx];
                        local_.back().node.keys.erase(
                            local_.back().node.keys.begin() + idx);
                        local_.back().node.values.erase(
                            local_.back().node.values.begin() + idx);
                    }
                }
            }
            handle_delete_underflow();
        } else if (extra_op == 'i' && extra_value) {
            if (!local_.empty() && local_.back().node.is_leaf) {
                int leaf_idx = static_cast<int>(local_.size()) - 1;
                {
                    auto& lf = local_[leaf_idx];
                    auto it = std::lower_bound(
                        lf.node.keys.begin(), lf.node.keys.end(), extra_key);
                    int pos = static_cast<int>(it - lf.node.keys.begin());
                    lf.node.keys.insert(it, extra_key);
                    if (lf.node.is_index_leaf)
                        lf.node.child_ids.insert(
                            lf.node.child_ids.begin() + pos,
                            bytes_to_int(*extra_value));
                    else
                        lf.node.values.insert(
                            lf.node.values.begin() + pos, *extra_value);
                }
                if (static_cast<int>(local_[leaf_idx].node.keys.size())
                    >= order_) {
                    int new_id = split_leaf(local_[leaf_idx]);
                    int parent_id = local_[leaf_idx].parent_id;
                    if (parent_id != INVALID_KEY) {
                        for (auto& p : local_) {
                            if (p.id == parent_id && !p.node.is_leaf) {
                                int ci = find_child_index(
                                    p.node,
                                    local_[leaf_idx].node.keys.back());
                                int sep = local_.back().node.keys[0];
                                p.node.keys.insert(
                                    p.node.keys.begin() + ci, sep);
                                int nlv = oram_.get_leaf(new_id);
                                p.node.child_ids.insert(
                                    p.node.child_ids.begin() + ci + 1,
                                    new_id);
                                p.node.child_leaves.insert(
                                    p.node.child_leaves.begin() + ci + 1,
                                    nlv);
                                break;
                            }
                        }
                    }
                }
            }
        }

        reassign_all_leaves();
        flush_all_to_stash();
        (void)extra_ops_used;
    }

    // Phase 3: pad remaining budget with dummies.
    do_dummy_ops(std::max(0, 3 * max_height_ - op_count_));
    finalize_bw();
    return result;
}

// ─── State export / import ─────────────────────────────────────────────────

Bytes BPlusOmap::export_state() const {
    Bytes buf;
    auto si = [&](int v){ size_t p=buf.size(); buf.resize(p+4); std::memcpy(buf.data()+p,&v,4); };
    si(root_id_); si(root_leaf_); si(next_block_id_); si(order_);
    si(max_height_); si(ods_mode_ ? 1 : 0); si(split_depth_);
    si(index_mode_ ? 1 : 0);

    auto b0 = oram_.export_state(-1);
    si(static_cast<int>(b0.size()));
    buf.insert(buf.end(), b0.begin(), b0.end());

    if (split_depth_ > 0) {
        auto b1 = upper_oram_.export_state(-1);
        si(static_cast<int>(b1.size()));
        buf.insert(buf.end(), b1.begin(), b1.end());
    }
    return buf;
}

std::unique_ptr<BPlusOmap> BPlusOmap::from_state(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v,p,4); p+=4; return v; };

    int root_id = di(), root_leaf = di(), next_block_id = di(),
        order = di(), max_height = di(), ods_mode = di(),
        split_depth = di(), idx_mode = di();

    int b0_len = di();
    (void)b0_len;
    auto oram = PathORAM::from_state_network(p, channel);

    auto bp = std::make_unique<BPlusOmap>(1, order, oram.bucket_size(), nullptr);
    bp->root_id_ = root_id;
    bp->root_leaf_ = root_leaf;
    bp->next_block_id_ = next_block_id;
    bp->max_height_ = max_height;
    bp->ods_mode_ = (ods_mode != 0);
    bp->split_depth_ = split_depth;
    bp->index_mode_ = (idx_mode != 0);
    bp->oram_ = std::move(oram);

    if (split_depth > 0) {
        int b1_len = di();
        (void)b1_len;
        bp->upper_oram_ = PathORAM::from_state_network(p, channel);
    }
    return bp;
}

}  // namespace tiered_omap
