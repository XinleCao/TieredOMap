#include "tiered_omap/omap/avl_omap.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tiered_omap {

// ─── AVLNodeData encoding ───────────────────────────────────────────────────

Bytes AVLNodeData::encode() const {
    Bytes result(AVL_HEADER_SIZE + data.size());
    uint8_t* p = result.data();
    auto put = [&](int v) { std::memcpy(p, &v, sizeof(int)); p += sizeof(int); };
    put(l_key); put(l_leaf); put(l_height);
    put(r_key); put(r_leaf); put(r_height);
    std::memcpy(p, data.data(), data.size());
    return result;
}

AVLNodeData AVLNodeData::decode(const Bytes& raw) {
    if (raw.size() < AVL_HEADER_SIZE)
        throw std::runtime_error("AVLNodeData::decode: buffer too small");
    AVLNodeData nd;
    const uint8_t* p = raw.data();
    auto get = [&]() { int v; std::memcpy(&v, p, sizeof(int)); p += sizeof(int); return v; };
    nd.l_key = get(); nd.l_leaf = get(); nd.l_height = get();
    nd.r_key = get(); nd.r_leaf = get(); nd.r_height = get();
    nd.data.assign(raw.begin() + AVL_HEADER_SIZE, raw.end());
    return nd;
}

// ─── Constructors ────────────────────────────────────────────────────────────

AVLOmap::AVLOmap(int capacity, int bucket_size)
    : capacity_(capacity),
      oram_(capacity, bucket_size) {
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(1.44 * std::log2(std::max(capacity, 2)))));
}

AVLOmap::AVLOmap(int capacity, int bucket_size, int split_depth, int upper_capacity)
    : capacity_(capacity),
      split_depth_(split_depth),
      upper_oram_(std::max(upper_capacity, 1), bucket_size),
      oram_(capacity, bucket_size) {
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(1.44 * std::log2(std::max(capacity, 2)))));
    if (split_depth_ > max_height_)
        split_depth_ = max_height_;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

PathORAM& AVLOmap::oram_for_depth(int depth) {
    return (split_depth_ > 0 && depth < split_depth_) ? upper_oram_ : oram_;
}

void AVLOmap::reset_op_counts() {
    op_count_ = 0;
    upper_op_count_ = 0;
    lower_op_count_ = 0;
}

// ─── Init: build balanced BST ───────────────────────────────────────────────

int AVLOmap::build_balanced(
    const std::vector<std::pair<int, Bytes>>& sorted,
    int lo, int hi,
    std::unordered_map<int, Bytes>& oram_data) {
    if (lo > hi) return INVALID_KEY;

    int mid = lo + (hi - lo) / 2;
    int user_key = sorted[mid].first;

    AVLNodeData nd;
    nd.data = sorted[mid].second;

    int l_key = build_balanced(sorted, lo, mid - 1, oram_data);
    int r_key = build_balanced(sorted, mid + 1, hi, oram_data);

    nd.l_key = l_key;
    nd.r_key = r_key;

    oram_data[user_key] = nd.encode();
    return user_key;
}

void AVLOmap::init(const std::vector<std::pair<int, Bytes>>& data) {
    if (data.empty()) {
        root_key_ = INVALID_KEY;
        root_leaf_ = INVALID_LEAF;
        return;
    }

    auto sorted = data;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });

    std::unordered_map<int, Bytes> oram_data;
    int root = build_balanced(sorted, 0,
                              static_cast<int>(sorted.size()) - 1, oram_data);

    std::unordered_map<int, AVLNodeData> nodes;
    for (auto& [k, v] : oram_data)
        nodes[k] = AVLNodeData::decode(v);

    std::function<int(int)> fix_heights = [&](int key) -> int {
        if (key == INVALID_KEY) return 0;
        auto& nd = nodes.at(key);
        nd.l_height = fix_heights(nd.l_key);
        nd.r_height = fix_heights(nd.r_key);
        return nd.height();
    };
    fix_heights(root);

    if (split_depth_ > 0) {
        // Split mode: assign nodes to upper or lower ORAM based on depth.
        upper_oram_ = PathORAM(std::max(upper_oram_.num_data(), 1),
                               upper_oram_.bucket_size());
        oram_ = PathORAM(capacity_, oram_.bucket_size());

        std::function<void(int, int)> assign_leaves = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (depth < split_depth_)
                upper_oram_.set_leaf(key, upper_oram_.random_leaf());
            else
                oram_.set_leaf(key, oram_.random_leaf());
            assign_leaves(nd.l_key, depth + 1);
            assign_leaves(nd.r_key, depth + 1);
        };
        assign_leaves(root, 0);

        // Fix child leaf pointers: child's leaf comes from the child's ORAM.
        std::function<void(int, int)> fix_child_leaves = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (nd.l_key != INVALID_KEY) {
                PathORAM& child_oram = (depth + 1 < split_depth_) ? upper_oram_ : oram_;
                nd.l_leaf = child_oram.get_leaf(nd.l_key);
            }
            if (nd.r_key != INVALID_KEY) {
                PathORAM& child_oram = (depth + 1 < split_depth_) ? upper_oram_ : oram_;
                nd.r_leaf = child_oram.get_leaf(nd.r_key);
            }
            fix_child_leaves(nd.l_key, depth + 1);
            fix_child_leaves(nd.r_key, depth + 1);
        };
        fix_child_leaves(root, 0);

        // Partition encoded data into upper and lower sets.
        std::unordered_map<int, Bytes> upper_data, lower_data;
        std::function<void(int, int)> partition = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            if (depth < split_depth_)
                upper_data[key] = nd.encode();
            else
                lower_data[key] = nd.encode();
            partition(nd.l_key, depth + 1);
            partition(nd.r_key, depth + 1);
        };
        partition(root, 0);

        upper_oram_.init(upper_data);
        oram_.init(lower_data);
    } else {
        // Non-split mode: single ORAM.
        oram_ = PathORAM(capacity_, oram_.bucket_size());
        for (auto& [k, _] : nodes)
            oram_.set_leaf(k, oram_.random_leaf());

        for (auto& [k, nd] : nodes) {
            if (nd.l_key != INVALID_KEY)
                nd.l_leaf = oram_.get_leaf(nd.l_key);
            if (nd.r_key != INVALID_KEY)
                nd.r_leaf = oram_.get_leaf(nd.r_key);
        }

        oram_data.clear();
        for (auto& [k, nd] : nodes)
            oram_data[k] = nd.encode();

        oram_.init(oram_data);
    }

    root_key_ = root;
    root_leaf_ = oram_for_depth(0).get_leaf(root);
}

// ─── Core operations ────────────────────────────────────────────────────────

void AVLOmap::move_to_local(int key, int leaf, int parent_key, int depth) {
    PathORAM& oram = oram_for_depth(depth);
    oram.read_path_to_stash(leaf);
    Block block = oram.extract_from_stash(key);
    AVLNodeData nd = AVLNodeData::decode(block.value);
    local_.push_back({key, block.leaf, nd, parent_key, depth});
    oram.evict_and_write_path(leaf);

    if (split_depth_ > 0) {
        if (depth < split_depth_) ++upper_op_count_;
        else ++lower_op_count_;
    } else {
        ++op_count_;
    }
}

void AVLOmap::flush_local_to_stash() {
    for (auto& node : local_) {
        Block block{node.key, node.leaf, node.avl.encode()};
        oram_for_depth(node.depth).add_to_stash(block);
    }
    local_.clear();
}

void AVLOmap::reassign_leaves() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& nd = local_[i];
        PathORAM& oram = oram_for_depth(nd.depth);
        int new_leaf = oram.random_leaf();
        oram.set_leaf(nd.key, new_leaf);
        nd.leaf = new_leaf;
    }

    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& child = local_[i];
        if (child.parent_key == INVALID_KEY) continue;
        for (auto& parent : local_) {
            if (parent.key != child.parent_key) continue;
            if (parent.avl.l_key == child.key)
                parent.avl.l_leaf = child.leaf;
            else if (parent.avl.r_key == child.key)
                parent.avl.r_leaf = child.leaf;
            break;
        }
    }

    for (auto& nd : local_) {
        if (nd.key == root_key_) {
            root_leaf_ = nd.leaf;
            break;
        }
    }
}

void AVLOmap::finalize_bw() {
    if (split_depth_ > 0) {
        int ubw = upper_oram_.path_bandwidth_bytes();
        int lbw = oram_.path_bandwidth_bytes();
        last_bw_.rounds = upper_op_count_ + lower_op_count_;
        last_bw_.bytes_downloaded =
            static_cast<uint64_t>(upper_op_count_) * ubw +
            static_cast<uint64_t>(lower_op_count_) * lbw;
        last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    } else {
        int bw = oram_.path_bandwidth_bytes();
        last_bw_.rounds = op_count_;
        last_bw_.bytes_downloaded = static_cast<uint64_t>(op_count_) * bw;
        last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    }
    total_bw_ += last_bw_;
}

// ─── Search ──────────────────────────────────────────────────────────────────

Bytes AVLOmap::search(int key, const Bytes* update) {
    last_bw_.reset();
    reset_op_counts();

    if (root_key_ == INVALID_KEY) {
        if (split_depth_ > 0) {
            int eff_split = std::min(split_depth_, max_height_);
            for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
            upper_op_count_ = eff_split;
            int lower_count = max_height_ - eff_split;
            for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
            lower_op_count_ = lower_count;
        } else {
            for (int i = 0; i < max_height_; ++i) oram_.dummy_access();
            op_count_ = max_height_;
        }
        finalize_bw();
        return {};
    }

    local_.clear();
    Bytes result;
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;
    int ops = 0;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
        ops++;
        auto& node = local_.back();

        if (key == cur_key) {
            result = node.avl.data;
            if (update) node.avl.data = *update;
            break;
        } else if (key < cur_key) {
            cur_key = node.avl.l_key;
            cur_leaf = node.avl.l_leaf;
        } else {
            cur_key = node.avl.r_key;
            cur_leaf = node.avl.r_leaf;
        }
    }

    // Pad remaining levels with dummy accesses.
    if (split_depth_ > 0) {
        int eff_split = std::min(split_depth_, max_height_);
        int upper_real = std::min(ops, eff_split);
        int lower_real = ops - upper_real;
        int upper_pad = eff_split - upper_real;
        int lower_pad = (max_height_ - eff_split) - lower_real;
        for (int i = 0; i < upper_pad; ++i) upper_oram_.dummy_access();
        upper_op_count_ += upper_pad;
        for (int i = 0; i < lower_pad; ++i) oram_.dummy_access();
        lower_op_count_ += lower_pad;
    } else {
        int pad = max_height_ - ops;
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        op_count_ += pad;
    }

    reassign_leaves();
    flush_local_to_stash();
    finalize_bw();
    return result;
}

// ─── Dummy Access ────────────────────────────────────────────────────────────

void AVLOmap::dummy_access() {
    last_bw_.reset();
    reset_op_counts();

    if (split_depth_ > 0) {
        int eff_split = std::min(split_depth_, max_height_);
        for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
        upper_op_count_ = eff_split;
        int lower_count = max_height_ - eff_split;
        for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
        lower_op_count_ = lower_count;
    } else {
        for (int i = 0; i < max_height_; ++i) oram_.dummy_access();
        op_count_ = max_height_;
    }
    finalize_bw();
}

void AVLOmap::partial_dummy_access() {
    if (split_depth_ <= 0) {
        dummy_access();
        return;
    }
    last_bw_.reset();
    reset_op_counts();

    int eff_split = std::min(split_depth_, max_height_);
    for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
    upper_op_count_ = eff_split;

    oram_.dummy_access();
    lower_op_count_ = 1;

    finalize_bw();
}

// ─── Insert ─────────────────────────────────────────────────────────────────

void AVLOmap::insert(int key, const Bytes& value) {
    last_bw_.reset();
    reset_op_counts();

    if (root_key_ == INVALID_KEY) {
        AVLNodeData nd;
        nd.data = value;
        PathORAM& root_oram = oram_for_depth(0);
        int leaf = root_oram.random_leaf();
        root_oram.set_leaf(key, leaf);
        root_oram.add_to_stash({key, leaf, nd.encode()});
        root_key_ = key;
        root_leaf_ = leaf;
        // Pad with dummy ops.
        if (split_depth_ > 0) {
            int eff_split = std::min(split_depth_, 2 * max_height_ + 1);
            for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
            upper_op_count_ = eff_split;
            int lower_count = (2 * max_height_ + 1) - eff_split;
            for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
            lower_op_count_ = lower_count;
        } else {
            for (int i = 0; i < 2 * max_height_ + 1; ++i) oram_.dummy_access();
            op_count_ = 2 * max_height_ + 1;
        }
        finalize_bw();
        return;
    }

    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;
    int ops = 0;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
        ops++;
        auto& node = local_.back();

        if (key < cur_key) {
            if (node.avl.l_key == INVALID_KEY) {
                AVLNodeData new_nd;
                new_nd.data = value;
                int child_depth = d + 1;
                PathORAM& child_oram = oram_for_depth(child_depth);
                int new_leaf = child_oram.random_leaf();
                child_oram.set_leaf(key, new_leaf);
                node.avl.l_key = key;
                node.avl.l_leaf = new_leaf;
                node.avl.l_height = 1;
                local_.push_back({key, new_leaf, new_nd, cur_key, child_depth});
                break;
            }
            cur_key = node.avl.l_key;
            cur_leaf = node.avl.l_leaf;
        } else {
            if (node.avl.r_key == INVALID_KEY) {
                AVLNodeData new_nd;
                new_nd.data = value;
                int child_depth = d + 1;
                PathORAM& child_oram = oram_for_depth(child_depth);
                int new_leaf = child_oram.random_leaf();
                child_oram.set_leaf(key, new_leaf);
                node.avl.r_key = key;
                node.avl.r_leaf = new_leaf;
                node.avl.r_height = 1;
                local_.push_back({key, new_leaf, new_nd, cur_key, child_depth});
                break;
            }
            cur_key = node.avl.r_key;
            cur_leaf = node.avl.r_leaf;
        }
    }

    update_heights();
    rebalance();
    reassign_leaves();
    flush_local_to_stash();

    int total_needed = 3 * max_height_ + 1;
    int pad = std::max(0, total_needed - ops * 2);
    if (split_depth_ > 0) {
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        lower_op_count_ += pad;
    } else {
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        op_count_ += pad;
    }
    finalize_bw();
}

// ─── Remove ─────────────────────────────────────────────────────────────────

void AVLOmap::remove(int key) {
    last_bw_.reset();
    reset_op_counts();

    if (root_key_ == INVALID_KEY) {
        if (split_depth_ > 0) {
            int eff_split = std::min(split_depth_, max_height_);
            for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
            upper_op_count_ = eff_split;
            int lower_count = max_height_ - eff_split;
            for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
            lower_op_count_ = lower_count;
        } else {
            for (int i = 0; i < max_height_; ++i) oram_.dummy_access();
            op_count_ = max_height_;
        }
        finalize_bw();
        return;
    }

    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;
    int ops = 0;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
        ops++;
        auto& node = local_.back();

        if (key == cur_key) {
            if (node.avl.l_key == INVALID_KEY && node.avl.r_key == INVALID_KEY) {
                if (node.parent_key != INVALID_KEY) {
                    for (auto& p : local_) {
                        if (p.key != node.parent_key) continue;
                        if (p.avl.l_key == key) {
                            p.avl.l_key = INVALID_KEY;
                            p.avl.l_leaf = INVALID_LEAF;
                            p.avl.l_height = 0;
                        } else {
                            p.avl.r_key = INVALID_KEY;
                            p.avl.r_leaf = INVALID_LEAF;
                            p.avl.r_height = 0;
                        }
                        break;
                    }
                } else {
                    root_key_ = INVALID_KEY;
                    root_leaf_ = INVALID_LEAF;
                }
                local_.pop_back();
            } else if (node.avl.l_key == INVALID_KEY ||
                       node.avl.r_key == INVALID_KEY) {
                int child_key = (node.avl.l_key != INVALID_KEY)
                                    ? node.avl.l_key : node.avl.r_key;
                int child_leaf = (node.avl.l_key != INVALID_KEY)
                                     ? node.avl.l_leaf : node.avl.r_leaf;
                if (node.parent_key != INVALID_KEY) {
                    for (auto& p : local_) {
                        if (p.key != node.parent_key) continue;
                        if (p.avl.l_key == key) {
                            p.avl.l_key = child_key;
                            p.avl.l_leaf = child_leaf;
                        } else {
                            p.avl.r_key = child_key;
                            p.avl.r_leaf = child_leaf;
                        }
                        break;
                    }
                } else {
                    root_key_ = child_key;
                    root_leaf_ = child_leaf;
                }
                local_.pop_back();
            } else {
                node.avl.data.clear();
            }
            break;
        } else if (key < cur_key) {
            cur_key = node.avl.l_key;
            cur_leaf = node.avl.l_leaf;
        } else {
            cur_key = node.avl.r_key;
            cur_leaf = node.avl.r_leaf;
        }
    }

    update_heights();
    reassign_leaves();
    flush_local_to_stash();

    int pad = std::max(0, 2 * max_height_ - ops);
    if (split_depth_ > 0) {
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        lower_op_count_ += pad;
    } else {
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        op_count_ += pad;
    }
    finalize_bw();
}

// ─── Rebalancing ────────────────────────────────────────────────────────────

void AVLOmap::update_heights() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& nd = local_[i];
        for (auto& child : local_) {
            if (child.key == nd.avl.l_key)
                nd.avl.l_height = child.avl.height();
            if (child.key == nd.avl.r_key)
                nd.avl.r_height = child.avl.height();
        }
    }
}

std::tuple<int, int, int> AVLOmap::rotate(int idx, bool left) {
    auto& node = local_[idx];
    int pivot_key = left ? node.avl.r_key : node.avl.l_key;

    int pivot_idx = -1;
    for (int i = 0; i < static_cast<int>(local_.size()); ++i)
        if (local_[i].key == pivot_key) { pivot_idx = i; break; }
    if (pivot_idx < 0) return {node.key, node.leaf, node.avl.height()};

    auto& pivot = local_[pivot_idx];

    if (left) {
        node.avl.r_key = pivot.avl.l_key;
        node.avl.r_leaf = pivot.avl.l_leaf;
        node.avl.r_height = pivot.avl.l_height;
        pivot.avl.l_key = node.key;
        pivot.avl.l_leaf = node.leaf;
        pivot.avl.l_height = node.avl.height();
    } else {
        node.avl.l_key = pivot.avl.r_key;
        node.avl.l_leaf = pivot.avl.r_leaf;
        node.avl.l_height = pivot.avl.r_height;
        pivot.avl.r_key = node.key;
        pivot.avl.r_leaf = node.leaf;
        pivot.avl.r_height = node.avl.height();
    }

    for (auto& p : local_) {
        if (p.key == node.parent_key) {
            if (p.avl.l_key == node.key) {
                p.avl.l_key = pivot.key;
                p.avl.l_leaf = pivot.leaf;
                p.avl.l_height = pivot.avl.height();
            } else if (p.avl.r_key == node.key) {
                p.avl.r_key = pivot.key;
                p.avl.r_leaf = pivot.leaf;
                p.avl.r_height = pivot.avl.height();
            }
            break;
        }
    }

    pivot.parent_key = node.parent_key;
    node.parent_key = pivot.key;

    if (root_key_ == node.key)
        root_key_ = pivot.key;

    return {pivot.key, pivot.leaf, pivot.avl.height()};
}

std::tuple<int, int, int> AVLOmap::balance_node(int idx) {
    auto& nd = local_[idx];
    int bal = nd.avl.balance();

    if (bal > 1) {
        for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
            if (local_[i].key == nd.avl.l_key && local_[i].avl.balance() < 0) {
                rotate(i, true);
                break;
            }
        }
        return rotate(idx, false);
    }
    if (bal < -1) {
        for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
            if (local_[i].key == nd.avl.r_key && local_[i].avl.balance() > 0) {
                rotate(i, false);
                break;
            }
        }
        return rotate(idx, true);
    }

    return {nd.key, nd.leaf, nd.avl.height()};
}

void AVLOmap::rebalance() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        update_heights();
        balance_node(i);
    }
    update_heights();
}

}  // namespace tiered_omap
