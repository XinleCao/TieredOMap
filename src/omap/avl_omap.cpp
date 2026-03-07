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
    oram.inject_round_delay();

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

    int budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        if (split_depth_ > 0) {
            int eff_split = std::min(split_depth_, budget);
            for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
            upper_op_count_ = eff_split;
            int lower_count = budget - eff_split;
            for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
            lower_op_count_ = lower_count;
        } else {
            for (int i = 0; i < budget; ++i) oram_.dummy_access();
            op_count_ = budget;
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

    reassign_leaves();
    flush_local_to_stash();

    if (!ods_mode_) {
        int pad = std::max(0, budget - ops);
        if (split_depth_ > 0) {
            for (int i = 0; i < pad; ++i) oram_.dummy_access();
            lower_op_count_ += pad;
        } else {
            for (int i = 0; i < pad; ++i) oram_.dummy_access();
            op_count_ += pad;
        }
    }
    finalize_bw();
    return result;
}

// ─── Dummy Access ────────────────────────────────────────────────────────────

void AVLOmap::dummy_access() {
    last_bw_.reset();
    reset_op_counts();

    int budget = 3 * max_height_;

    if (split_depth_ > 0) {
        int eff_split = std::min(split_depth_, budget);
        for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
        upper_op_count_ = eff_split;
        int lower_count = budget - eff_split;
        for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
        lower_op_count_ = lower_count;
    } else {
        for (int i = 0; i < budget; ++i) oram_.dummy_access();
        op_count_ = budget;
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

    int eff_split = std::min(split_depth_, 3 * max_height_);
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

    int budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        AVLNodeData nd;
        nd.data = value;
        PathORAM& root_oram = oram_for_depth(0);
        int leaf = root_oram.random_leaf();
        root_oram.set_leaf(key, leaf);
        root_oram.add_to_stash({key, leaf, nd.encode()});
        root_key_ = key;
        root_leaf_ = leaf;
        if (split_depth_ > 0) {
            int eff_split = std::min(split_depth_, budget);
            for (int i = 0; i < eff_split; ++i) upper_oram_.dummy_access();
            upper_op_count_ = eff_split;
            int lower_count = budget - eff_split;
            for (int i = 0; i < lower_count; ++i) oram_.dummy_access();
            lower_op_count_ = lower_count;
        } else {
            for (int i = 0; i < budget; ++i) oram_.dummy_access();
            op_count_ = budget;
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

    if (!ods_mode_) {
        int pad = std::max(0, budget - ops);
        if (split_depth_ > 0) {
            for (int i = 0; i < pad; ++i) oram_.dummy_access();
            lower_op_count_ += pad;
        } else {
            for (int i = 0; i < pad; ++i) oram_.dummy_access();
            op_count_ += pad;
        }
    }
    finalize_bw();
}

// ─── Remove ─────────────────────────────────────────────────────────────────

void AVLOmap::remove(int key) {
    last_bw_.reset();
    reset_op_counts();

    int budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        for (int i = 0; i < budget; ++i) oram_.dummy_access();
        op_count_ = budget;
        finalize_bw();
        return;
    }

    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;
    int phase1_ops = 0;
    int target_idx = -1;

    // Phase 1: traverse down to find key.
    bool two_child = false;
    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;
        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
        phase1_ops++;

        int last_idx = static_cast<int>(local_.size()) - 1;
        int nk = local_[last_idx].key;

        if (key == cur_key) {
            target_idx = last_idx;
            if (local_[last_idx].avl.l_key != INVALID_KEY &&
                local_[last_idx].avl.r_key != INVALID_KEY) {
                two_child = true;
                int s_key = local_[last_idx].avl.r_key;
                int s_leaf = local_[last_idx].avl.r_leaf;
                for (int sd = d + 1; sd < max_height_; ++sd) {
                    int sp = local_.back().key;
                    move_to_local(s_key, s_leaf, sp, sd);
                    phase1_ops++;
                    int si = static_cast<int>(local_.size()) - 1;
                    if (local_[si].avl.l_key == INVALID_KEY) break;
                    s_key = local_[si].avl.l_key;
                    s_leaf = local_[si].avl.l_leaf;
                }
            }
            break;
        } else if (key < nk) {
            cur_key = local_[last_idx].avl.l_key;
            cur_leaf = local_[last_idx].avl.l_leaf;
        } else {
            cur_key = local_[last_idx].avl.r_key;
            cur_leaf = local_[last_idx].avl.r_leaf;
        }
    }

    // Perform deletion.
    if (target_idx >= 0 &&
        target_idx < static_cast<int>(local_.size())) {

        if (two_child) {
            int succ_idx = static_cast<int>(local_.size()) - 1;
            int succ_key = local_[succ_idx].key;
            Bytes succ_data = local_[succ_idx].avl.data;

            // Rename target node: key changes from old to successor's key.
            int old_key = local_[target_idx].key;
            local_[target_idx].key = succ_key;
            local_[target_idx].avl.data = succ_data;

            // Update parent's child pointer to the renamed key.
            int par = local_[target_idx].parent_key;
            if (par != INVALID_KEY) {
                for (auto& p : local_) {
                    if (p.key != par) continue;
                    if (p.avl.l_key == old_key) {
                        p.avl.l_key = succ_key;
                    } else if (p.avl.r_key == old_key) {
                        p.avl.r_key = succ_key;
                    }
                    break;
                }
            }
            if (root_key_ == old_key) root_key_ = succ_key;

            // Also update children's parent_key references.
            for (auto& c : local_) {
                if (c.parent_key == old_key) c.parent_key = succ_key;
            }

            // Now delete the successor position (0 or 1 child).
            target_idx = succ_idx;
        }

        int del_key = local_[target_idx].key;
        int child_key = (local_[target_idx].avl.l_key != INVALID_KEY)
                            ? local_[target_idx].avl.l_key
                            : local_[target_idx].avl.r_key;
        int child_leaf = (local_[target_idx].avl.l_key != INVALID_KEY)
                             ? local_[target_idx].avl.l_leaf
                             : local_[target_idx].avl.r_leaf;
        int child_h = (local_[target_idx].avl.l_key != INVALID_KEY)
                          ? local_[target_idx].avl.l_height
                          : local_[target_idx].avl.r_height;
        if (child_key == INVALID_KEY) child_h = 0;

        int del_parent = local_[target_idx].parent_key;
        if (del_parent != INVALID_KEY) {
            for (auto& p : local_) {
                if (p.key != del_parent) continue;
                if (p.avl.l_key == del_key) {
                    p.avl.l_key = child_key;
                    p.avl.l_leaf = child_leaf;
                    p.avl.l_height = child_h;
                } else if (p.avl.r_key == del_key) {
                    p.avl.r_key = child_key;
                    p.avl.r_leaf = child_leaf;
                    p.avl.r_height = child_h;
                }
                break;
            }
        } else {
            root_key_ = child_key;
            root_leaf_ = (child_key != INVALID_KEY) ? child_leaf
                                                     : INVALID_LEAF;
        }
        local_.erase(local_.begin() + target_idx);
    }

    // Save path node keys (bottom to top) for Phase 2 iteration.
    std::vector<int> path_keys;
    path_keys.reserve(local_.size());
    for (auto& ln : local_) path_keys.push_back(ln.key);

    // Phase 2: back-up rebalancing — read siblings bottom-to-top.
    int phase2_ops = 0;
    for (int pi = static_cast<int>(path_keys.size()) - 1; pi >= 0; --pi) {
        int nd_key = path_keys[pi];
        int nd_idx = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == nd_key) { nd_idx = j; break; }
        if (nd_idx < 0) {
            oram_.dummy_access();
            oram_.dummy_access();
            phase2_ops += 2;
            if (split_depth_ == 0) op_count_ += 2;
            continue;
        }

        // Refresh child heights from local_ nodes.
        for (auto& c : local_) {
            if (c.key == local_[nd_idx].avl.l_key)
                local_[nd_idx].avl.l_height = c.avl.height();
            if (c.key == local_[nd_idx].avl.r_key)
                local_[nd_idx].avl.r_height = c.avl.height();
        }

        int bal = local_[nd_idx].avl.balance();
        int nd_depth = local_[nd_idx].depth;

        if (std::abs(bal) <= 1) {
            oram_for_depth(nd_depth).dummy_access();
            oram_for_depth(nd_depth).dummy_access();
            phase2_ops += 2;
            if (split_depth_ == 0) op_count_ += 2;
            continue;
        }

        // Unbalanced — cache values before move_to_local may invalidate.
        int tall_key = (bal > 1) ? local_[nd_idx].avl.l_key
                                 : local_[nd_idx].avl.r_key;
        int tall_leaf = (bal > 1) ? local_[nd_idx].avl.l_leaf
                                  : local_[nd_idx].avl.r_leaf;

        bool tall_in_local = false;
        for (auto& n : local_)
            if (n.key == tall_key) { tall_in_local = true; break; }

        if (tall_key != INVALID_KEY && !tall_in_local) {
            move_to_local(tall_key, tall_leaf, nd_key, nd_depth + 1);
        } else {
            oram_for_depth(nd_depth).dummy_access();
            if (split_depth_ == 0) ++op_count_;
        }
        phase2_ops++;

        // Check if double rotation is needed.
        int tall_idx = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == tall_key) { tall_idx = j; break; }

        bool need_inner = false;
        int inner_key = INVALID_KEY, inner_leaf = INVALID_LEAF;
        int tall_depth_val = nd_depth + 1;
        if (tall_idx >= 0) {
            tall_depth_val = local_[tall_idx].depth;
            int tb_bal = local_[tall_idx].avl.balance();
            if (bal > 1 && tb_bal < 0) {
                inner_key = local_[tall_idx].avl.r_key;
                inner_leaf = local_[tall_idx].avl.r_leaf;
                need_inner = true;
            } else if (bal < -1 && tb_bal > 0) {
                inner_key = local_[tall_idx].avl.l_key;
                inner_leaf = local_[tall_idx].avl.l_leaf;
                need_inner = true;
            }
        }

        if (need_inner && inner_key != INVALID_KEY) {
            bool inner_in_local = false;
            for (auto& n : local_)
                if (n.key == inner_key) { inner_in_local = true; break; }
            if (!inner_in_local) {
                move_to_local(inner_key, inner_leaf,
                              tall_key, tall_depth_val + 1);
            } else {
                oram_for_depth(nd_depth).dummy_access();
                if (split_depth_ == 0) ++op_count_;
            }
        } else {
            oram_for_depth(nd_depth).dummy_access();
            if (split_depth_ == 0) ++op_count_;
        }
        phase2_ops++;

        // Rebalance: refresh heights then rotate.
        update_heights();
        int ni = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == nd_key) { ni = j; break; }
        if (ni >= 0) balance_node(ni);
        update_heights();
    }

    reassign_leaves();
    flush_local_to_stash();

    if (!ods_mode_) {
        int total_ops = phase1_ops + phase2_ops;
        int pad = std::max(0, budget - total_ops);
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        if (split_depth_ == 0) op_count_ += pad;
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

    int transferred_key = INVALID_KEY;
    if (left) {
        transferred_key = pivot.avl.l_key;
        node.avl.r_key = pivot.avl.l_key;
        node.avl.r_leaf = pivot.avl.l_leaf;
        node.avl.r_height = pivot.avl.l_height;
        pivot.avl.l_key = node.key;
        pivot.avl.l_leaf = node.leaf;
        pivot.avl.l_height = node.avl.height();
    } else {
        transferred_key = pivot.avl.r_key;
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

    if (transferred_key != INVALID_KEY) {
        for (auto& c : local_) {
            if (c.key == transferred_key) {
                c.parent_key = node.key;
                break;
            }
        }
    }

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
