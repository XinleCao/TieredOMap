#include "tiered_omap/omap/bplus_omap.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tiered_omap {

// ─── BPlusNode encoding ────────────────────────────────────────────────────
// Format: [is_leaf:1][num_keys:4][keys...][children or values...]
// Internal: after keys → [child_id:4, child_leaf:4] × (num_keys+1)
// Leaf:     after keys → [val_size:4, val_data...] × num_keys

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

BPlusOmap::BPlusOmap(int capacity, int order, int bucket_size)
    : capacity_(capacity),
      order_(order),
      oram_(capacity, bucket_size) {
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

    // Step 1: Create leaf nodes.
    std::vector<std::pair<int, int>> level_ids;  // (smallest_key, oram_id)
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

    // Step 2: Build internal levels bottom-up until one root remains.
    int max_children = order_;
    while (level_ids.size() > 1) {
        std::vector<std::pair<int, int>> next_level;
        for (size_t i = 0; i < level_ids.size(); ) {
            BPlusNode internal;
            internal.is_leaf = false;
            size_t end = std::min(i + max_children, level_ids.size());
            for (size_t j = i; j < end; ++j) {
                internal.child_ids.push_back(level_ids[j].second);
                internal.child_leaves.push_back(INVALID_LEAF);  // fixed later
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

    // Decode all nodes, assign leaves from ORAM pos_map, fix child_leaves.
    std::unordered_map<int, BPlusNode> nodes;
    for (auto& [k, v] : oram_data)
        nodes[k] = BPlusNode::decode(v);

    oram_ = PathORAM(std::max(next_block_id_, 1), oram_.bucket_size());
    for (auto& [k, _] : nodes)
        oram_.set_leaf(k, oram_.random_leaf());

    // Fix child_leaves in internal nodes.
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

// ─── Core operations ────────────────────────────────────────────────────────

int BPlusOmap::find_child_index(const BPlusNode& node, int key) {
    // Returns index i such that keys[i-1] <= key < keys[i].
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

void BPlusOmap::flush_local_to_stash() {
    for (auto& ln : local_) {
        Block block{ln.id, ln.leaf, ln.node.encode()};
        oram_.add_to_stash(block);
    }
    local_.clear();
}

void BPlusOmap::reassign_leaves() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& ln = local_[i];
        int new_leaf = oram_.random_leaf();
        oram_.set_leaf(ln.id, new_leaf);
        ln.leaf = new_leaf;
    }

    // Fix child_leaves in parent nodes.
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& child = local_[i];
        if (child.parent_id == INVALID_KEY) continue;
        for (auto& parent : local_) {
            if (parent.id != child.parent_id) continue;
            if (parent.node.is_leaf) break;
            for (size_t j = 0; j < parent.node.child_ids.size(); ++j) {
                if (parent.node.child_ids[j] == child.id) {
                    parent.node.child_leaves[j] = child.leaf;
                    break;
                }
            }
            break;
        }
    }

    // Update root_leaf_.
    for (auto& ln : local_) {
        if (ln.id == root_id_) {
            root_leaf_ = ln.leaf;
            break;
        }
    }
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

Bytes BPlusOmap::search(int key, const Bytes* update) {
    last_bw_.reset();
    op_count_ = 0;

    if (root_id_ == INVALID_KEY) {
        do_dummy_ops(max_height_);
        finalize_bw();
        return {};
    }

    local_.clear();
    Bytes result;
    int cur_id = root_id_;
    int cur_leaf = root_leaf_;
    int ops = 0;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_id == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().id;
        move_to_local(cur_id, cur_leaf, parent);
        ops++;
        auto& ln = local_.back();

        if (ln.node.is_leaf) {
            int idx = find_leaf_index(ln.node, key);
            if (idx >= 0) {
                result = ln.node.values[idx];
                if (update) ln.node.values[idx] = *update;
            }
            break;
        } else {
            int ci = find_child_index(ln.node, key);
            cur_id = ln.node.child_ids[ci];
            cur_leaf = ln.node.child_leaves[ci];
        }
    }

    do_dummy_ops(max_height_ - ops);
    reassign_leaves();
    flush_local_to_stash();

    finalize_bw();
    return result;
}

void BPlusOmap::dummy_access() {
    last_bw_.reset();
    op_count_ = 0;
    do_dummy_ops(max_height_);
    finalize_bw();
}

// ─── Insert (simplified: in-place if leaf has room, otherwise no-op) ────────

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
        do_dummy_ops(max_height_);
        finalize_bw();
        return;
    }

    // Traverse to the target leaf.
    local_.clear();
    int cur_id = root_id_;
    int cur_leaf = root_leaf_;
    int ops = 0;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_id == INVALID_KEY) break;
        int parent = local_.empty() ? INVALID_KEY : local_.back().id;
        move_to_local(cur_id, cur_leaf, parent);
        ops++;
        auto& ln = local_.back();

        if (ln.node.is_leaf) {
            // Insert in sorted order.
            auto it = std::lower_bound(ln.node.keys.begin(),
                                       ln.node.keys.end(), key);
            int pos = static_cast<int>(it - ln.node.keys.begin());
            ln.node.keys.insert(it, key);
            ln.node.values.insert(ln.node.values.begin() + pos, value);

            // If full, split.
            if (static_cast<int>(ln.node.keys.size()) >= order_) {
                int new_id = split_leaf(ln);
                // Propagate split up to parent if needed.
                if (ln.parent_id != INVALID_KEY) {
                    for (auto& p : local_) {
                        if (p.id == ln.parent_id && !p.node.is_leaf) {
                            // Find position and insert new child.
                            int ci = find_child_index(p.node, ln.node.keys.back());
                            // The new sibling's smallest key becomes a separator.
                            BPlusNode& new_sib_node = local_.back().node;
                            int sep = new_sib_node.keys[0];
                            p.node.keys.insert(
                                p.node.keys.begin() + ci, sep);
                            int new_leaf = oram_.get_leaf(new_id);
                            p.node.child_ids.insert(
                                p.node.child_ids.begin() + ci + 1, new_id);
                            p.node.child_leaves.insert(
                                p.node.child_leaves.begin() + ci + 1, new_leaf);
                            break;
                        }
                    }
                } else {
                    // Split at root: create new root.
                    BPlusNode new_root;
                    new_root.is_leaf = false;
                    BPlusNode& new_sib = local_.back().node;
                    new_root.keys = {new_sib.keys[0]};
                    new_root.child_ids = {ln.id, new_id};
                    new_root.child_leaves = {ln.leaf, oram_.get_leaf(new_id)};
                    int rid = next_block_id_++;
                    int rlf = oram_.random_leaf();
                    oram_.set_leaf(rid, rlf);
                    local_.push_back({rid, rlf, new_root, INVALID_KEY});
                    root_id_ = rid;
                }
            }
            break;
        } else {
            int ci = find_child_index(ln.node, key);
            cur_id = ln.node.child_ids[ci];
            cur_leaf = ln.node.child_leaves[ci];
        }
    }

    reassign_leaves();
    flush_local_to_stash();
    do_dummy_ops(std::max(0, 2 * max_height_ - ops));
    finalize_bw();
}

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

void BPlusOmap::remove(int key) {
    search(key);
}

}  // namespace tiered_omap
