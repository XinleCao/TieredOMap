#include "tiered_omap/omap/bplus_omap.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tiered_omap {

// ─── BPlusNode encoding ────────────────────────────────────────────────────

Bytes BPlusNode::encode() const {
    Bytes buf;
    buf.push_back(is_leaf ? 1 : 0);

    auto push_int = [&](int v) {
        buf.resize(buf.size() + sizeof(int));
        std::memcpy(&buf[buf.size() - sizeof(int)], &v, sizeof(int));
    };

    push_int(static_cast<int>(keys.size()));
    for (int k : keys) push_int(k);

    if (!is_leaf) {
        for (size_t i = 0; i < child_ids.size(); ++i) {
            push_int(child_ids[i]);
            push_int(child_leaves[i]);
        }
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
    nd.is_leaf = (raw[pos++] == 1);

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

    if (!nd.is_leaf) {
        int nc = nk + 1;
        nd.child_ids.resize(nc);
        nd.child_leaves.resize(nc);
        for (int i = 0; i < nc; ++i) {
            nd.child_ids[i] = get_int();
            nd.child_leaves[i] = get_int();
        }
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

// ─── Build B+ tree bottom-up ────────────────────────────────────────────────

int BPlusOmap::build_tree(
    const std::vector<std::pair<int, Bytes>>& sorted,
    std::unordered_map<int, Bytes>& oram_data) {
    if (sorted.empty()) return INVALID_KEY;

    std::vector<std::pair<int, int>> level_ids;
    int max_leaf_keys = order_ - 1;

    for (size_t i = 0; i < sorted.size(); ) {
        BPlusNode leaf;
        leaf.is_leaf = true;
        size_t end = std::min(i + max_leaf_keys, sorted.size());
        for (size_t j = i; j < end; ++j) {
            leaf.keys.push_back(sorted[j].first);
            leaf.values.push_back(sorted[j].second);
        }
        int id = next_block_id_++;
        oram_data[id] = leaf.encode();
        level_ids.push_back({leaf.keys[0], id});
        i = end;
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
    root_id_ = root;
    root_leaf_ = oram_.get_leaf(root);
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

void BPlusOmap::move_to_local(int id, int leaf, int parent_id) {
    oram_.read_path_to_stash(leaf);
    Block block = oram_.extract_from_stash(id);
    BPlusNode nd = BPlusNode::decode(block.value);
    local_.push_back({id, block.leaf, nd, parent_id});
    oram_.evict_and_write_path(leaf);
    ++op_count_;
}

void BPlusOmap::move_to_sibling_cache(int id, int leaf,
                                       int parent_local_idx, int child_idx) {
    oram_.read_path_to_stash(leaf);
    Block block = oram_.extract_from_stash(id);
    BPlusNode nd = BPlusNode::decode(block.value);
    sibling_cache_.push_back({id, block.leaf, nd, parent_local_idx, child_idx});
    oram_.evict_and_write_path(leaf);
    ++op_count_;
}

void BPlusOmap::do_dummy_ops(int count) {
    for (int i = 0; i < count; ++i)
        oram_.dummy_access();
    op_count_ += count;
}

void BPlusOmap::finalize_bw() {
    int bw = oram_.path_bandwidth_bytes();
    last_bw_.rounds = op_count_;
    last_bw_.bytes_downloaded = static_cast<uint64_t>(op_count_) * bw;
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;
}

int BPlusOmap::min_leaf_keys() const {
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
        move_to_local(cur_id, cur_leaf, parent);
        auto& ln = local_.back();

        if (ln.node.is_leaf) {
            oram_.dummy_access();
            op_count_++;
            break;
        }

        int ci = find_child_index(ln.node, key);
        int num_ch = static_cast<int>(ln.node.child_ids.size());
        int sib_idx = (ci > 0) ? ci - 1 : ((ci + 1 < num_ch) ? ci + 1 : -1);

        if (sib_idx >= 0) {
            int ploc = static_cast<int>(local_.size()) - 1;
            move_to_sibling_cache(
                ln.node.child_ids[sib_idx],
                ln.node.child_leaves[sib_idx],
                ploc, sib_idx);
        } else {
            oram_.dummy_access();
            op_count_++;
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
        int nl = oram_.random_leaf();
        oram_.set_leaf(ln.id, nl);
        ln.leaf = nl;
    }

    for (auto& sib : sibling_cache_) {
        if (sib.id == INVALID_KEY) continue;
        int nl = oram_.random_leaf();
        oram_.set_leaf(sib.id, nl);
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
        oram_.add_to_stash(block);
    }
    for (auto& sib : sibling_cache_) {
        if (sib.id == INVALID_KEY) continue;
        Block block{sib.id, sib.leaf, sib.node.encode()};
        oram_.add_to_stash(block);
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
                if (sib_left) {
                    nd.node.keys.insert(nd.node.keys.begin(), sib->node.keys.back());
                    nd.node.values.insert(nd.node.values.begin(), sib->node.values.back());
                    sib->node.keys.pop_back();
                    sib->node.values.pop_back();
                    par.node.keys[sep] = nd.node.keys[0];
                } else {
                    nd.node.keys.push_back(sib->node.keys[0]);
                    nd.node.values.push_back(sib->node.values[0]);
                    sib->node.keys.erase(sib->node.keys.begin());
                    sib->node.values.erase(sib->node.values.begin());
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
                if (sib_left) {
                    sib->node.keys.insert(sib->node.keys.end(),
                        nd.node.keys.begin(), nd.node.keys.end());
                    sib->node.values.insert(sib->node.values.end(),
                        nd.node.values.begin(), nd.node.values.end());
                    par.node.keys.erase(par.node.keys.begin() + sep);
                    par.node.child_ids.erase(par.node.child_ids.begin() + nd_idx);
                    par.node.child_leaves.erase(par.node.child_leaves.begin() + nd_idx);
                    nd.node.keys.clear();
                    nd.node.values.clear();
                } else {
                    nd.node.keys.insert(nd.node.keys.end(),
                        sib->node.keys.begin(), sib->node.keys.end());
                    nd.node.values.insert(nd.node.values.end(),
                        sib->node.values.begin(), sib->node.values.end());
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
    new_leaf.keys.assign(nd.keys.begin() + mid, nd.keys.end());
    new_leaf.values.assign(nd.values.begin() + mid, nd.values.end());

    nd.keys.resize(mid);
    nd.values.resize(mid);

    int new_id = next_block_id_++;
    int new_lf = oram_.random_leaf();
    oram_.set_leaf(new_id, new_lf);
    local_.push_back({new_id, new_lf, new_leaf, leaf_ln.parent_id});
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
    int new_lf = oram_.random_leaf();
    oram_.set_leaf(new_id, new_lf);
    local_.push_back({new_id, new_lf, new_internal, int_ln.parent_id});
    return new_id;
}

// ═══════════════════════════════════════════════════════════════════════════
// All three operations use traverse_with_siblings + pad to 2*max_height_
// so the server sees an identical ORAM access pattern regardless of op type.
// ═══════════════════════════════════════════════════════════════════════════

Bytes BPlusOmap::search(int key, const Bytes* update) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        if (!ods_mode_) do_dummy_ops(2 * max_height_);
        finalize_bw();
        return {};
    }

    traverse_with_siblings(key);

    Bytes result;
    if (!local_.empty() && local_.back().node.is_leaf) {
        int idx = find_leaf_index(local_.back().node, key);
        if (idx >= 0) {
            result = local_.back().node.values[idx];
            if (update) local_.back().node.values[idx] = *update;
        }
    }

    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) do_dummy_ops(std::max(0, 2 * max_height_ - op_count_));
    finalize_bw();
    return result;
}

void BPlusOmap::insert(int key, const Bytes& value) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        BPlusNode leaf;
        leaf.is_leaf = true;
        leaf.keys = {key};
        leaf.values = {value};
        int id = next_block_id_++;
        int lf = oram_.random_leaf();
        oram_.set_leaf(id, lf);
        oram_.add_to_stash({id, lf, leaf.encode()});
        root_id_ = id;
        root_leaf_ = lf;
        if (!ods_mode_) do_dummy_ops(2 * max_height_);
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
            leaf_ln.node.values.insert(leaf_ln.node.values.begin() + pos, value);
        }

        if (static_cast<int>(local_[leaf_idx].node.keys.size()) >= order_) {
            int new_id = split_leaf(local_[leaf_idx]);
            // split_leaf pushes to local_, possibly reallocating the buffer.
            // All prior references into local_ are now invalid; use indices.
            int parent_id = local_[leaf_idx].parent_id;
            if (parent_id != INVALID_KEY) {
                for (auto& p : local_) {
                    if (p.id == parent_id && !p.node.is_leaf) {
                        int ci = find_child_index(
                            p.node, local_[leaf_idx].node.keys.back());
                        int sep_key = local_.back().node.keys[0];
                        p.node.keys.insert(p.node.keys.begin() + ci, sep_key);
                        int new_leaf_val = oram_.get_leaf(new_id);
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
                new_root.child_leaves = {old_leaf, oram_.get_leaf(new_id)};
                int rid = next_block_id_++;
                int rlf = oram_.random_leaf();
                oram_.set_leaf(rid, rlf);
                local_.push_back({rid, rlf, new_root, INVALID_KEY});
                root_id_ = rid;
            }
        }
    }

    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) do_dummy_ops(std::max(0, 2 * max_height_ - op_count_));
    finalize_bw();
}

void BPlusOmap::remove(int key) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        if (!ods_mode_) do_dummy_ops(2 * max_height_);
        finalize_bw();
        return;
    }

    traverse_with_siblings(key);

    if (!local_.empty() && local_.back().node.is_leaf) {
        int idx = find_leaf_index(local_.back().node, key);
        if (idx >= 0) {
            local_.back().node.keys.erase(local_.back().node.keys.begin() + idx);
            local_.back().node.values.erase(local_.back().node.values.begin() + idx);
        }
    }

    handle_delete_underflow();
    reassign_all_leaves();
    flush_all_to_stash();
    if (!ods_mode_) do_dummy_ops(std::max(0, 2 * max_height_ - op_count_));
    finalize_bw();
}

void BPlusOmap::dummy_access() {
    last_bw_.reset();
    op_count_ = 0;
    if (!ods_mode_) do_dummy_ops(2 * max_height_);
    finalize_bw();
}

Bytes BPlusOmap::search_piggyback(int key, const Bytes* update,
                                   int extra_key, char extra_op,
                                   const Bytes* extra_value,
                                   Bytes* extra_result) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        do_dummy_ops(2 * max_height_);
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
            result = local_.back().node.values[idx];
            if (update) local_.back().node.values[idx] = *update;
        }
    }

    // Flush main traversal to stash so the ORAM is ready for phase 2.
    reassign_all_leaves();
    flush_all_to_stash();
    int main_ops = op_count_;

    // Phase 2: piggyback — traverse for extra_key within remaining budget.
    int budget = 2 * max_height_ - main_ops;
    if (extra_key != INVALID_KEY && budget > 0) {
        traverse_with_siblings(extra_key);
        int extra_ops_used = op_count_ - main_ops;

        if (extra_op == 's' && extra_result) {
            // Scan-read: just return the value.
            if (!local_.empty() && local_.back().node.is_leaf) {
                int idx = find_leaf_index(local_.back().node, extra_key);
                if (idx >= 0)
                    *extra_result = local_.back().node.values[idx];
            }
        } else if (extra_op == 'd') {
            // Delete the extra key from its leaf.
            if (!local_.empty() && local_.back().node.is_leaf) {
                int idx = find_leaf_index(local_.back().node, extra_key);
                if (idx >= 0) {
                    if (extra_result)
                        *extra_result = local_.back().node.values[idx];
                    local_.back().node.keys.erase(
                        local_.back().node.keys.begin() + idx);
                    local_.back().node.values.erase(
                        local_.back().node.values.begin() + idx);
                }
            }
            handle_delete_underflow();
        } else if (extra_op == 'i' && extra_value) {
            // Insert the extra key into the correct leaf.
            if (!local_.empty() && local_.back().node.is_leaf) {
                int leaf_idx = static_cast<int>(local_.size()) - 1;
                {
                    auto& lf = local_[leaf_idx];
                    auto it = std::lower_bound(
                        lf.node.keys.begin(), lf.node.keys.end(), extra_key);
                    int pos = static_cast<int>(it - lf.node.keys.begin());
                    lf.node.keys.insert(it, extra_key);
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
    do_dummy_ops(std::max(0, 2 * max_height_ - op_count_));
    finalize_bw();
    return result;
}

}  // namespace tiered_omap
