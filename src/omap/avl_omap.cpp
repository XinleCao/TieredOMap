#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/tcp_channel.h"
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

AVLOmap::AVLOmap(int capacity, int bucket_size, StorageCreator storage_creator)
    : capacity_(capacity),
      storage_creator_(std::move(storage_creator)),
      oram_(capacity, bucket_size, 7, storage_creator_) {
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(1.44 * std::log2(std::max(capacity, 2)))));
}

AVLOmap::AVLOmap(int capacity, int bucket_size, int split_depth,
                 int upper_capacity, StorageCreator storage_creator)
    : capacity_(capacity),
      split_depth_(split_depth),
      storage_creator_(std::move(storage_creator)),
      upper_oram_(std::max(upper_capacity, 1), bucket_size, 7, storage_creator_),
      oram_(capacity, bucket_size, 7, storage_creator_) {
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
                               upper_oram_.bucket_size(), 7, storage_creator_);
        oram_ = PathORAM(capacity_, oram_.bucket_size(), 7, storage_creator_);

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
        oram_ = PathORAM(capacity_, oram_.bucket_size(), 7, storage_creator_);
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

void AVLOmap::tracked_dummy(int depth) {
    oram_for_depth(depth).dummy_access();
    if (split_depth_ > 0) {
        if (depth < split_depth_) ++upper_op_count_;
        else ++lower_op_count_;
    } else {
        ++op_count_;
    }
}

void AVLOmap::pad_to_budget(int budget) {
    if (split_depth_ > 0) {
        int upper_budget = std::min(split_depth_, budget);
        int upper_pad = std::max(0, upper_budget - upper_op_count_);
        for (int i = 0; i < upper_pad; ++i) upper_oram_.dummy_access();
        upper_op_count_ += upper_pad;

        int lower_budget = budget - upper_budget;
        int lower_pad = std::max(0, lower_budget - lower_op_count_);
        for (int i = 0; i < lower_pad; ++i) oram_.dummy_access();
        lower_op_count_ += lower_pad;
    } else {
        int total = upper_op_count_ + lower_op_count_ + op_count_;
        int pad = std::max(0, budget - total);
        for (int i = 0; i < pad; ++i) oram_.dummy_access();
        op_count_ += pad;
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
        if (!ods_mode_) pad_to_budget(budget);
        finalize_bw();
        return {};
    }

    local_.clear();
    Bytes result;
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
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

    if (!ods_mode_) pad_to_budget(budget);
    finalize_bw();
    return result;
}

// ─── Dummy Access ────────────────────────────────────────────────────────────

void AVLOmap::dummy_access() {
    last_bw_.reset();
    reset_op_counts();
    pad_to_budget(3 * max_height_);
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
        if (!ods_mode_) pad_to_budget(budget);
        finalize_bw();
        return;
    }

    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;

        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
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

    if (!ods_mode_) pad_to_budget(budget);
    finalize_bw();
}

// ─── Remove ─────────────────────────────────────────────────────────────────

void AVLOmap::remove(int key) {
    last_bw_.reset();
    reset_op_counts();

    int budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        if (!ods_mode_) pad_to_budget(budget);
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
            tracked_dummy(split_depth_);
            tracked_dummy(split_depth_);
            phase2_ops += 2;
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
            tracked_dummy(nd_depth);
            tracked_dummy(nd_depth);
            phase2_ops += 2;
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
            tracked_dummy(nd_depth + 1);
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
                tracked_dummy(tall_depth_val + 1);
            }
        } else {
            tracked_dummy(tall_depth_val + 1);
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

    if (!ods_mode_) pad_to_budget(budget);
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

// ─── Step-by-step interface ──────────────────────────────────────────────────

void AVLOmap::begin_step_search(int key, const Bytes* update) {
    last_bw_.reset();
    reset_op_counts();
    local_.clear();
    pb_ = PBState{};

    ss_ = StepState{};
    ss_.key = key;
    ss_.update = update;
    ss_.budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        ss_.phase = StepPhase::PAD;
        ss_.pad_remaining = ss_.budget;
        if (split_depth_ > 0)
            ss_.dummy_split_boundary = std::min(split_depth_, ss_.budget);
        else
            ss_.dummy_split_boundary = ss_.budget;
        ss_.dummy_step = 0;
    } else {
        ss_.phase = StepPhase::TRAVERSE;
        ss_.cur_key = root_key_;
        ss_.cur_leaf = root_leaf_;
        ss_.depth = 0;
    }
}

void AVLOmap::begin_step_dummy() {
    last_bw_.reset();
    reset_op_counts();
    local_.clear();
    pb_ = PBState{};

    ss_ = StepState{};
    ss_.is_dummy = true;
    ss_.budget = 3 * max_height_;
    ss_.phase = StepPhase::PAD;
    ss_.pad_remaining = ss_.budget;
    ss_.dummy_step = 0;
    if (split_depth_ > 0)
        ss_.dummy_split_boundary = std::min(split_depth_, ss_.budget);
    else
        ss_.dummy_split_boundary = ss_.budget;
}

void AVLOmap::begin_step_partial_dummy() {
    if (split_depth_ <= 0) {
        begin_step_dummy();
        return;
    }
    last_bw_.reset();
    reset_op_counts();
    local_.clear();
    pb_ = PBState{};

    int eff_split = std::min(split_depth_, 3 * max_height_);
    ss_ = StepState{};
    ss_.is_dummy = true;
    ss_.budget = eff_split + 1;
    ss_.phase = StepPhase::PAD;
    ss_.pad_remaining = eff_split + 1;
    ss_.dummy_step = 0;
    ss_.dummy_split_boundary = eff_split;
}

OramStepRound AVLOmap::step_next_round() {
    OramStepRound r;
    ss_.round_read = (ss_.phase != StepPhase::DONE);
    pb_.round_read = (pb_.active && pb_.phase != StepPhase::DONE);

    // ── Main operation read ──
    if (ss_.round_read) {
        PathORAM* oram = nullptr;
        int leaf = INVALID_LEAF;
        if (ss_.phase == StepPhase::TRAVERSE) {
            oram = &oram_for_depth(ss_.depth);
            leaf = ss_.cur_leaf;
        } else {
            if (split_depth_ > 0)
                oram = (ss_.dummy_step < ss_.dummy_split_boundary)
                           ? &upper_oram_ : &oram_;
            else
                oram = &oram_;
            leaf = oram->random_leaf();
        }
        ss_.cur_round_oram = oram;
        ss_.cur_round_leaf = leaf;
        r.reads.push_back({oram->get_store_id(), leaf});
    }

    // ── Piggyback operation read ──
    if (pb_.round_read) {
        PathORAM* oram = nullptr;
        int leaf = INVALID_LEAF;
        if (pb_.phase == StepPhase::TRAVERSE) {
            pb_.node_in_local = false;
            for (auto& ln : local_)
                if (ln.key == pb_.cur_key) { pb_.node_in_local = true; break; }
            oram = &oram_for_depth(pb_.depth);
            leaf = pb_.node_in_local ? oram->random_leaf() : pb_.cur_leaf;
        } else {
            if (split_depth_ > 0)
                oram = (pb_.dummy_step < pb_.dummy_split_boundary)
                           ? &upper_oram_ : &oram_;
            else
                oram = &oram_;
            leaf = oram->random_leaf();
        }
        pb_.cur_round_oram = oram;
        pb_.cur_round_leaf = leaf;
        r.reads.push_back({oram->get_store_id(), leaf});
    }

    return r;
}

void AVLOmap::step_apply_reads(const std::vector<PathData>& results) {
    if (ss_.round_read && pb_.round_read && results.size() >= 2) {
        if (ss_.cur_round_oram == pb_.cur_round_oram) {
            PathData merged(results[0]);
            for (auto& [nid, blocks] : results[1])
                if (merged.find(nid) == merged.end())
                    merged[nid] = blocks;
            ss_.cur_round_oram->apply_fetched_path(std::move(merged));
        } else {
            ss_.cur_round_oram->apply_fetched_path(PathData(results[0]));
            pb_.cur_round_oram->apply_fetched_path(PathData(results[1]));
        }
    } else if (ss_.round_read && !results.empty()) {
        ss_.cur_round_oram->apply_fetched_path(PathData(results[0]));
    } else if (pb_.round_read && !results.empty()) {
        pb_.cur_round_oram->apply_fetched_path(PathData(results[0]));
    }
}

std::vector<StepWriteReq> AVLOmap::step_prepare_writes() {
    std::vector<StepWriteReq> writes;

    if (!ss_.round_read && !pb_.round_read)
        return writes;

    if (ss_.round_read && pb_.round_read) {
        if (ss_.cur_round_oram == pb_.cur_round_oram) {
            writes.push_back({ss_.cur_round_oram->get_store_id(),
                              ss_.cur_round_oram->prepare_eviction_paths(
                                  {ss_.cur_round_leaf, pb_.cur_round_leaf})});
        } else {
            writes.push_back({ss_.cur_round_oram->get_store_id(),
                              ss_.cur_round_oram->prepare_eviction(ss_.cur_round_leaf)});
            writes.push_back({pb_.cur_round_oram->get_store_id(),
                              pb_.cur_round_oram->prepare_eviction(pb_.cur_round_leaf)});
        }
    } else if (ss_.round_read) {
        writes.push_back({ss_.cur_round_oram->get_store_id(),
                          ss_.cur_round_oram->prepare_eviction(ss_.cur_round_leaf)});
    } else {
        writes.push_back({pb_.cur_round_oram->get_store_id(),
                          pb_.cur_round_oram->prepare_eviction(pb_.cur_round_leaf)});
    }
    return writes;
}

void AVLOmap::step_process() {
    // ── Main operation ──
    if (ss_.phase == StepPhase::TRAVERSE) {
        Block block = ss_.cur_round_oram->extract_from_stash(ss_.cur_key);
        AVLNodeData nd = AVLNodeData::decode(block.value);
        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        local_.push_back({ss_.cur_key, block.leaf, nd, parent, ss_.depth});
        ss_.ops++;

        if (split_depth_ > 0) {
            if (ss_.depth < split_depth_) ++upper_op_count_;
            else ++lower_op_count_;
        } else {
            ++op_count_;
        }

        bool found = (ss_.key == ss_.cur_key);
        if (found) {
            ss_.result = nd.data;
            if (ss_.update) local_.back().avl.data = *ss_.update;
        }

        bool end_traverse = found
            || (ss_.cur_key == INVALID_KEY)
            || (ss_.depth >= max_height_ - 1);

        if (!found && !end_traverse) {
            auto& node = local_.back();
            if (ss_.key < ss_.cur_key) {
                ss_.cur_key = node.avl.l_key;
                ss_.cur_leaf = node.avl.l_leaf;
            } else {
                ss_.cur_key = node.avl.r_key;
                ss_.cur_leaf = node.avl.r_leaf;
            }
            if (ss_.cur_key == INVALID_KEY)
                end_traverse = true;
        }

        ss_.depth++;

        if (end_traverse) {
            reassign_leaves();
            flush_local_to_stash();
            ss_.phase = StepPhase::PAD;
            ss_.pad_remaining = std::max(0, ss_.budget - ss_.ops);
            ss_.dummy_step = 0;
            if (split_depth_ > 0)
                ss_.dummy_split_boundary = std::max(0,
                    std::min(split_depth_, ss_.budget) - upper_op_count_);
            else
                ss_.dummy_split_boundary = ss_.pad_remaining;
            if (ss_.pad_remaining == 0)
                ss_.phase = StepPhase::DONE;
        }
    } else if (ss_.phase == StepPhase::PAD) {
        ss_.ops++;
        ss_.dummy_step++;
        ss_.pad_remaining--;

        if (split_depth_ > 0) {
            if (ss_.dummy_step <= ss_.dummy_split_boundary)
                ++upper_op_count_;
            else
                ++lower_op_count_;
        } else {
            ++op_count_;
        }

        if (ss_.pad_remaining <= 0)
            ss_.phase = StepPhase::DONE;
    }

    // ── Piggyback operation ──
    if (!pb_.active || pb_.phase == StepPhase::DONE)
        return;

    if (pb_.phase == StepPhase::TRAVERSE) {
        int local_idx = -1;
        if (pb_.node_in_local) {
            for (int i = 0; i < static_cast<int>(local_.size()); ++i)
                if (local_[i].key == pb_.cur_key) { local_idx = i; break; }
            if (local_idx < 0) {
                // Main flushed local_; re-extract from stash
                PathORAM& po = oram_for_depth(pb_.depth);
                Block block = po.extract_from_stash(pb_.cur_key);
                AVLNodeData nd = AVLNodeData::decode(block.value);
                int parent = INVALID_KEY;
                for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
                    if (local_[i].avl.l_key == pb_.cur_key ||
                        local_[i].avl.r_key == pb_.cur_key) {
                        parent = local_[i].key; break;
                    }
                }
                local_.push_back({pb_.cur_key, block.leaf, nd, parent, pb_.depth});
                local_idx = static_cast<int>(local_.size()) - 1;
            }
        } else {
            // Main op may have added this node to local_ in the same round
            for (int i = 0; i < static_cast<int>(local_.size()); ++i)
                if (local_[i].key == pb_.cur_key) { local_idx = i; break; }
            if (local_idx < 0) {
                Block block = pb_.cur_round_oram->extract_from_stash(pb_.cur_key);
                AVLNodeData nd = AVLNodeData::decode(block.value);
                int parent = INVALID_KEY;
                for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
                    if (local_[i].avl.l_key == pb_.cur_key ||
                        local_[i].avl.r_key == pb_.cur_key) {
                        parent = local_[i].key; break;
                    }
                }
                local_.push_back({pb_.cur_key, block.leaf, nd, parent, pb_.depth});
                local_idx = static_cast<int>(local_.size()) - 1;
            }
        }
        pb_.ops++;
        if (split_depth_ > 0) {
            if (pb_.depth < split_depth_) ++upper_op_count_;
            else ++lower_op_count_;
        } else {
            ++op_count_;
        }

        if (local_idx >= 0) {
            auto& node = local_[local_idx];
            bool found = (pb_.key == pb_.cur_key);
            bool end_pb = false;

            if (pb_.is_insert) {
                if (found) {
                    node.avl.data = pb_.insert_value;
                    end_pb = true;
                } else if (pb_.key < pb_.cur_key) {
                    if (node.avl.l_key == INVALID_KEY) {
                        AVLNodeData new_nd;
                        new_nd.data = pb_.insert_value;
                        int cd = pb_.depth + 1;
                        PathORAM& co = oram_for_depth(cd);
                        int nlf = co.random_leaf();
                        co.set_leaf(pb_.key, nlf);
                        node.avl.l_key = pb_.key;
                        node.avl.l_leaf = nlf;
                        node.avl.l_height = 1;
                        local_.push_back({pb_.key, nlf, new_nd, pb_.cur_key, cd});
                        end_pb = true;
                    } else {
                        pb_.cur_key = node.avl.l_key;
                        pb_.cur_leaf = node.avl.l_leaf;
                    }
                } else {
                    if (node.avl.r_key == INVALID_KEY) {
                        AVLNodeData new_nd;
                        new_nd.data = pb_.insert_value;
                        int cd = pb_.depth + 1;
                        PathORAM& co = oram_for_depth(cd);
                        int nlf = co.random_leaf();
                        co.set_leaf(pb_.key, nlf);
                        node.avl.r_key = pb_.key;
                        node.avl.r_leaf = nlf;
                        node.avl.r_height = 1;
                        local_.push_back({pb_.key, nlf, new_nd, pb_.cur_key, cd});
                        end_pb = true;
                    } else {
                        pb_.cur_key = node.avl.r_key;
                        pb_.cur_leaf = node.avl.r_leaf;
                    }
                }
                if (!end_pb && (pb_.cur_key == INVALID_KEY ||
                                pb_.depth >= max_height_ - 1))
                    end_pb = true;
            } else {
                if (found) pb_.result = node.avl.data;
                end_pb = found || pb_.depth >= max_height_ - 1;
                if (!found && !end_pb) {
                    if (pb_.key < pb_.cur_key) {
                        pb_.cur_key = node.avl.l_key;
                        pb_.cur_leaf = node.avl.l_leaf;
                    } else {
                        pb_.cur_key = node.avl.r_key;
                        pb_.cur_leaf = node.avl.r_leaf;
                    }
                    if (pb_.cur_key == INVALID_KEY) end_pb = true;
                }
            }

            pb_.depth++;
            if (end_pb) {
                if (pb_.is_insert) {
                    update_heights();
                    rebalance();
                }
                pb_.traverse_done = true;
                pb_.phase = StepPhase::PAD;
                pb_.pad_remaining = std::max(0, pb_.budget - pb_.ops);
                pb_.dummy_step = 0;
                pb_.dummy_split_boundary = pb_.pad_remaining;
                if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
            }
        }
    } else if (pb_.phase == StepPhase::PAD) {
        pb_.ops++;
        if (split_depth_ > 0) {
            if (pb_.dummy_step < pb_.dummy_split_boundary)
                ++upper_op_count_;
            else
                ++lower_op_count_;
        } else {
            ++op_count_;
        }
        pb_.dummy_step++;
        pb_.pad_remaining--;
        if (pb_.pad_remaining <= 0) pb_.phase = StepPhase::DONE;
    }
}

bool AVLOmap::step_done() const {
    if (pb_.active && pb_.phase != StepPhase::DONE)
        return false;
    return ss_.phase == StepPhase::DONE;
}

Bytes AVLOmap::step_finish() {
    finalize_bw();
    return ss_.result;
}

// ─── Piggyback step interface ───────────────────────────────────────────────

void AVLOmap::begin_piggyback_search(int key) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.key = key;
    pb_.budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        pb_.phase = StepPhase::PAD;
        pb_.pad_remaining = pb_.budget;
        pb_.traverse_done = true;
        if (split_depth_ > 0)
            pb_.dummy_split_boundary = std::min(split_depth_, pb_.budget);
        else
            pb_.dummy_split_boundary = pb_.budget;
        if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
    } else {
        pb_.phase = StepPhase::TRAVERSE;
        pb_.cur_key = root_key_;
        pb_.cur_leaf = root_leaf_;
        pb_.depth = 0;
    }
}

void AVLOmap::begin_piggyback_insert(int key, const Bytes& value) {
    pb_ = PBState{};
    pb_.active = true;
    pb_.key = key;
    pb_.is_insert = true;
    pb_.insert_value = value;
    pb_.budget = 3 * max_height_;

    if (root_key_ == INVALID_KEY) {
        AVLNodeData nd;
        nd.data = value;
        PathORAM& root_oram = oram_for_depth(0);
        int leaf = root_oram.random_leaf();
        root_oram.set_leaf(key, leaf);
        local_.push_back({key, leaf, nd, INVALID_KEY, 0});
        root_key_ = key;
        root_leaf_ = leaf;
        pb_.phase = StepPhase::PAD;
        pb_.pad_remaining = pb_.budget;
        pb_.traverse_done = true;
        if (split_depth_ > 0)
            pb_.dummy_split_boundary = std::min(split_depth_, pb_.budget);
        else
            pb_.dummy_split_boundary = pb_.budget;
        if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
    } else {
        pb_.phase = StepPhase::TRAVERSE;
        pb_.cur_key = root_key_;
        pb_.cur_leaf = root_leaf_;
        pb_.depth = 0;
    }
}

void AVLOmap::begin_piggyback_dummy() {
    pb_ = PBState{};
    pb_.active = true;
    pb_.phase = StepPhase::PAD;
    pb_.budget = 3 * max_height_;
    pb_.pad_remaining = pb_.budget;
    pb_.traverse_done = true;
    pb_.dummy_step = 0;
    if (split_depth_ > 0)
        pb_.dummy_split_boundary = std::min(split_depth_, pb_.budget);
    else
        pb_.dummy_split_boundary = pb_.budget;
    if (pb_.pad_remaining == 0) pb_.phase = StepPhase::DONE;
}

Bytes AVLOmap::finish_piggyback() {
    return pb_.result;
}

bool AVLOmap::piggyback_needs_decision() const {
    return pb_.active && pb_.traverse_done && !pb_.result.empty();
}

Bytes AVLOmap::piggyback_get_traverse_result() {
    return pb_.result;
}

void AVLOmap::piggyback_commit_remove() {
    // Removal is handled externally (separate OMAP call).
    // The pb continues padding within its 3h budget.
}

void AVLOmap::piggyback_commit_noop() {
    // Nothing to do — pb is already padding.
}

// ─── Non-step piggyback ─────────────────────────────────────────────────────

Bytes AVLOmap::search_piggyback(int key, const Bytes* update,
                                int extra_key, char extra_op,
                                const Bytes* extra_value,
                                Bytes* extra_result) {
    last_bw_.reset();
    reset_op_counts();

    int budget = 3 * max_height_;
    Bytes main_result;

    if (root_key_ == INVALID_KEY) {
        pad_to_budget(budget);
        int saved_u = upper_op_count_, saved_l = lower_op_count_;
        int saved_o = op_count_;
        reset_op_counts();
        pad_to_budget(budget);
        upper_op_count_ += saved_u;
        lower_op_count_ += saved_l;
        op_count_ += saved_o;
        finalize_bw();
        return {};
    }

    // ── Phase A: main search ──
    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;
        int parent = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, parent, d);
        auto& node = local_.back();
        if (key == cur_key) {
            main_result = node.avl.data;
            if (update) node.avl.data = *update;
            break;
        }
        if (key < cur_key) {
            cur_key = node.avl.l_key; cur_leaf = node.avl.l_leaf;
        } else {
            cur_key = node.avl.r_key; cur_leaf = node.avl.r_leaf;
        }
    }

    reassign_leaves();
    flush_local_to_stash();
    pad_to_budget(budget);

    int saved_u = upper_op_count_, saved_l = lower_op_count_;
    int saved_o = op_count_;
    reset_op_counts();

    // ── Phase B: extra operation ──
    local_.clear();
    cur_key = root_key_;
    cur_leaf = root_leaf_;
    Bytes pb_result;
    int pb_target_idx = -1;

    if (extra_op == 's' || extra_op == 'd') {
        for (int d = 0; d < max_height_; ++d) {
            if (cur_key == INVALID_KEY) break;
            int parent = local_.empty() ? INVALID_KEY : local_.back().key;
            move_to_local(cur_key, cur_leaf, parent, d);
            auto& node = local_.back();
            if (extra_key == cur_key) {
                pb_result = node.avl.data;
                pb_target_idx = static_cast<int>(local_.size()) - 1;
                break;
            }
            if (extra_key < cur_key) {
                cur_key = node.avl.l_key; cur_leaf = node.avl.l_leaf;
            } else {
                cur_key = node.avl.r_key; cur_leaf = node.avl.r_leaf;
            }
        }

        if (extra_result) *extra_result = pb_result;

        if (extra_op == 'd' && pb_target_idx >= 0) {
            auto& tgt = local_[pb_target_idx];
            bool two_child = tgt.avl.l_key != INVALID_KEY &&
                             tgt.avl.r_key != INVALID_KEY;
            if (two_child) {
                int sk = tgt.avl.r_key, sl = tgt.avl.r_leaf;
                for (int sd = tgt.depth + 1; sd < max_height_; ++sd) {
                    int sp = local_.back().key;
                    move_to_local(sk, sl, sp, sd);
                    int si = static_cast<int>(local_.size()) - 1;
                    if (local_[si].avl.l_key == INVALID_KEY) break;
                    sk = local_[si].avl.l_key;
                    sl = local_[si].avl.l_leaf;
                }
                int succ_idx = static_cast<int>(local_.size()) - 1;
                int old_key = local_[pb_target_idx].key;
                local_[pb_target_idx].key = local_[succ_idx].key;
                local_[pb_target_idx].avl.data = local_[succ_idx].avl.data;
                int par = local_[pb_target_idx].parent_key;
                if (par != INVALID_KEY) {
                    for (auto& p : local_) {
                        if (p.key != par) continue;
                        if (p.avl.l_key == old_key) p.avl.l_key = local_[pb_target_idx].key;
                        else if (p.avl.r_key == old_key) p.avl.r_key = local_[pb_target_idx].key;
                        break;
                    }
                }
                if (root_key_ == old_key) root_key_ = local_[pb_target_idx].key;
                for (auto& c : local_)
                    if (c.parent_key == old_key) c.parent_key = local_[pb_target_idx].key;
                pb_target_idx = succ_idx;
            }
            int dk = local_[pb_target_idx].key;
            int ck = (local_[pb_target_idx].avl.l_key != INVALID_KEY)
                         ? local_[pb_target_idx].avl.l_key
                         : local_[pb_target_idx].avl.r_key;
            int cl = (local_[pb_target_idx].avl.l_key != INVALID_KEY)
                         ? local_[pb_target_idx].avl.l_leaf
                         : local_[pb_target_idx].avl.r_leaf;
            int ch = (local_[pb_target_idx].avl.l_key != INVALID_KEY)
                         ? local_[pb_target_idx].avl.l_height
                         : local_[pb_target_idx].avl.r_height;
            if (ck == INVALID_KEY) ch = 0;
            int dp = local_[pb_target_idx].parent_key;
            if (dp != INVALID_KEY) {
                for (auto& p : local_) {
                    if (p.key != dp) continue;
                    if (p.avl.l_key == dk)      { p.avl.l_key = ck; p.avl.l_leaf = cl; p.avl.l_height = ch; }
                    else if (p.avl.r_key == dk) { p.avl.r_key = ck; p.avl.r_leaf = cl; p.avl.r_height = ch; }
                    break;
                }
            } else {
                root_key_ = ck;
                root_leaf_ = (ck != INVALID_KEY) ? cl : INVALID_LEAF;
            }
            local_.erase(local_.begin() + pb_target_idx);
            update_heights();
            rebalance();
        }
        reassign_leaves();
        flush_local_to_stash();
    } else if (extra_op == 'i' && extra_value) {
        // Insert extra_key (reuse insert logic)
        if (root_key_ == INVALID_KEY) {
            AVLNodeData nd; nd.data = *extra_value;
            PathORAM& roram = oram_for_depth(0);
            int leaf = roram.random_leaf();
            roram.set_leaf(extra_key, leaf);
            roram.add_to_stash({extra_key, leaf, nd.encode()});
            root_key_ = extra_key; root_leaf_ = leaf;
        } else {
            for (int d = 0; d < max_height_; ++d) {
                if (cur_key == INVALID_KEY) break;
                int parent = local_.empty() ? INVALID_KEY : local_.back().key;
                move_to_local(cur_key, cur_leaf, parent, d);
                auto& node = local_.back();
                if (extra_key < cur_key) {
                    if (node.avl.l_key == INVALID_KEY) {
                        AVLNodeData nd; nd.data = *extra_value;
                        int cd = d + 1;
                        PathORAM& co = oram_for_depth(cd);
                        int nl = co.random_leaf();
                        co.set_leaf(extra_key, nl);
                        node.avl.l_key = extra_key; node.avl.l_leaf = nl;
                        node.avl.l_height = 1;
                        local_.push_back({extra_key, nl, nd, cur_key, cd});
                        break;
                    }
                    cur_key = node.avl.l_key; cur_leaf = node.avl.l_leaf;
                } else {
                    if (node.avl.r_key == INVALID_KEY) {
                        AVLNodeData nd; nd.data = *extra_value;
                        int cd = d + 1;
                        PathORAM& co = oram_for_depth(cd);
                        int nl = co.random_leaf();
                        co.set_leaf(extra_key, nl);
                        node.avl.r_key = extra_key; node.avl.r_leaf = nl;
                        node.avl.r_height = 1;
                        local_.push_back({extra_key, nl, nd, cur_key, cd});
                        break;
                    }
                    cur_key = node.avl.r_key; cur_leaf = node.avl.r_leaf;
                }
            }
            update_heights();
            rebalance();
            reassign_leaves();
            flush_local_to_stash();
        }
    }

    pad_to_budget(budget);
    upper_op_count_ += saved_u;
    lower_op_count_ += saved_l;
    op_count_ += saved_o;

    finalize_bw();
    return main_result;
}

// ─── State export / import ─────────────────────────────────────────────────

Bytes AVLOmap::export_state() const {
    Bytes buf;
    auto si = [&](int v){ size_t p=buf.size(); buf.resize(p+4); std::memcpy(buf.data()+p,&v,4); };
    si(capacity_); si(max_height_); si(root_key_); si(root_leaf_); si(split_depth_);
    si(ods_mode_ ? 1 : 0);

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

std::unique_ptr<AVLOmap> AVLOmap::from_state(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v,p,4); p+=4; return v; };

    int capacity = di(), max_height = di(), root_key = di(),
        root_leaf = di(), split_depth = di(), ods_mode = di();

    int b0_len = di();
    (void)b0_len;
    auto oram = PathORAM::from_state_network(p, channel);

    auto avl = std::make_unique<AVLOmap>(1, oram.bucket_size(), nullptr);
    avl->capacity_ = capacity;
    avl->max_height_ = max_height;
    avl->root_key_ = root_key;
    avl->root_leaf_ = root_leaf;
    avl->split_depth_ = split_depth;
    avl->ods_mode_ = (ods_mode != 0);
    avl->oram_ = std::move(oram);

    if (split_depth > 0) {
        int b1_len = di();
        (void)b1_len;
        avl->upper_oram_ = PathORAM::from_state_network(p, channel);
    }
    return avl;
}

}  // namespace tiered_omap
