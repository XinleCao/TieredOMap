#include "tiered_omap/tee/tee_avl_omap.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace tiered_omap {
namespace tee {

// Node value in ORAM = AVL_HEADER_SIZE + user_value_size.
static int node_oram_val_size(int user_val_size) {
    return AVL_HEADER_SIZE + user_val_size;
}

TeeAvlOmap::TeeAvlOmap(int capacity, int value_size, int bucket_size,
                        int split_depth)
    : split_depth_(split_depth) {
    max_height_ = std::max(1,
        static_cast<int>(std::ceil(1.44 * std::log2(std::max(capacity, 2)))));
    if (split_depth_ > max_height_) split_depth_ = max_height_;

    int nv = node_oram_val_size(value_size);

    if (split_depth_ > 0) {
        int upper_cap = (1 << split_depth_) - 1;
        upper_oram_ = EnclaveOram(std::max(upper_cap, 1), nv, bucket_size);
    }
    lower_oram_ = EnclaveOram(capacity, nv, bucket_size);
}

EnclaveOram& TeeAvlOmap::oram_for_depth(int depth) {
    return (split_depth_ > 0 && depth < split_depth_) ? upper_oram_ : lower_oram_;
}

// ── Init ────────────────────────────────────────────────────────────────────

void TeeAvlOmap::init(const std::vector<std::pair<int, Bytes>>& data) {
    if (data.empty()) {
        root_key_ = INVALID_KEY;
        root_leaf_ = INVALID_LEAF;
        return;
    }

    auto sorted = data;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });

    // Build balanced BST.
    std::unordered_map<int, AVLNodeData> nodes;

    std::function<int(int, int)> build = [&](int lo, int hi) -> int {
        if (lo > hi) return INVALID_KEY;
        int mid = lo + (hi - lo) / 2;
        AVLNodeData nd;
        nd.data = sorted[mid].second;
        nd.l_key = build(lo, mid - 1);
        nd.r_key = build(mid + 1, hi);
        nodes[sorted[mid].first] = nd;
        return sorted[mid].first;
    };
    int root = build(0, static_cast<int>(sorted.size()) - 1);

    // Fix heights.
    std::function<int(int)> fix_h = [&](int key) -> int {
        if (key == INVALID_KEY) return 0;
        auto& nd = nodes.at(key);
        nd.l_height = fix_h(nd.l_key);
        nd.r_height = fix_h(nd.r_key);
        return nd.height();
    };
    fix_h(root);

    // Assign leaves based on split depth.
    std::function<void(int, int)> assign = [&](int key, int depth) {
        if (key == INVALID_KEY) return;
        auto& oram = oram_for_depth(depth);
        oram.set_leaf(key, oram.random_leaf());
        auto& nd = nodes.at(key);
        assign(nd.l_key, depth + 1);
        assign(nd.r_key, depth + 1);
    };
    assign(root, 0);

    // Fix child leaf pointers.
    std::function<void(int, int)> fix_leaves = [&](int key, int depth) {
        if (key == INVALID_KEY) return;
        auto& nd = nodes.at(key);
        if (nd.l_key != INVALID_KEY) {
            auto& co = oram_for_depth(depth + 1);
            nd.l_leaf = co.get_leaf(nd.l_key);
        }
        if (nd.r_key != INVALID_KEY) {
            auto& co = oram_for_depth(depth + 1);
            nd.r_leaf = co.get_leaf(nd.r_key);
        }
        fix_leaves(nd.l_key, depth + 1);
        fix_leaves(nd.r_key, depth + 1);
    };
    fix_leaves(root, 0);

    // Partition into upper/lower and init ORAMs.
    if (split_depth_ > 0) {
        std::vector<std::pair<int, Bytes>> upper_data, lower_data;
        std::function<void(int, int)> partition = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            Bytes enc = nd.encode();
            if (depth < split_depth_)
                upper_data.push_back({key, enc});
            else
                lower_data.push_back({key, enc});
            partition(nd.l_key, depth + 1);
            partition(nd.r_key, depth + 1);
        };
        partition(root, 0);

        if (!upper_data.empty()) upper_oram_.init(upper_data);
        if (!lower_data.empty()) lower_oram_.init(lower_data);
    } else {
        std::vector<std::pair<int, Bytes>> all;
        for (auto& [k, nd] : nodes) all.push_back({k, nd.encode()});
        lower_oram_.init(all);
    }

    root_key_ = root;
    root_leaf_ = oram_for_depth(0).get_leaf(root);
}

// ── Stats helpers ───────────────────────────────────────────────────────────

void TeeAvlOmap::begin_stats() {
    upper_ops_ = lower_ops_ = 0;
    if (split_depth_ > 0) upper_oram_.begin_page_tracking();
    lower_oram_.begin_page_tracking();
}

void TeeAvlOmap::end_stats() {
    last_stats_.upper_pages = split_depth_ > 0 ? upper_oram_.end_page_tracking() : 0;
    last_stats_.lower_pages = lower_oram_.end_page_tracking();
}

// ── Core helpers ────────────────────────────────────────────────────────────

void TeeAvlOmap::move_to_local(int key, int leaf, int parent_key, int depth) {
    auto& oram = oram_for_depth(depth);
    oram.read_path_to_stash(leaf);
    Block blk = oram.extract_from_stash(key);
    if (blk.key == INVALID_KEY)
        throw std::runtime_error("TeeAvlOmap: key " + std::to_string(key) +
                                 " not found in ORAM stash");
    AVLNodeData nd = AVLNodeData::decode(blk.value);
    local_.push_back({key, blk.leaf, nd, parent_key, depth});
    oram.evict_one_path(leaf);

    if (split_depth_ > 0 && depth < split_depth_)
        ++upper_ops_;
    else
        ++lower_ops_;
}

void TeeAvlOmap::flush_local_to_stash() {
    for (auto& node : local_) {
        auto& oram = oram_for_depth(node.depth);
        oram.add_to_stash(node.key, node.leaf, node.avl.encode());
    }
    local_.clear();
}

void TeeAvlOmap::reassign_leaves() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& nd = local_[i];
        auto& oram = oram_for_depth(nd.depth);
        int new_leaf = oram.random_leaf();
        oram.set_leaf(nd.key, new_leaf);
        nd.leaf = new_leaf;
    }

    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& child = local_[i];
        if (child.parent_key == INVALID_KEY) continue;
        for (auto& par : local_) {
            if (par.key != child.parent_key) continue;
            if (par.avl.l_key == child.key)
                par.avl.l_leaf = child.leaf;
            else if (par.avl.r_key == child.key)
                par.avl.r_leaf = child.leaf;
            break;
        }
    }

    for (auto& nd : local_)
        if (nd.key == root_key_) { root_leaf_ = nd.leaf; break; }
}

void TeeAvlOmap::pad_ops(int ops_done) {
    int budget = 3 * max_height_;
    int pad = std::max(0, budget - ops_done);
    if (split_depth_ > 0) {
        int eff_upper = std::min(split_depth_, budget);
        int upper_pad = std::max(0, eff_upper - upper_ops_);
        for (int i = 0; i < upper_pad; ++i) upper_oram_.dummy_access();
        int lower_pad = std::max(0, pad - upper_pad);
        for (int i = 0; i < lower_pad; ++i) lower_oram_.dummy_access();
    } else {
        for (int i = 0; i < pad; ++i) lower_oram_.dummy_access();
    }
}

// ── Search ──────────────────────────────────────────────────────────────────

Bytes TeeAvlOmap::search(int key, const Bytes* update) {
    begin_stats();

    if (root_key_ == INVALID_KEY) {
        pad_ops(0);
        end_stats();
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
        ++ops;
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
    pad_ops(ops);
    end_stats();
    return result;
}

// ── Insert ──────────────────────────────────────────────────────────────────

void TeeAvlOmap::insert(int key, const Bytes& value) {
    begin_stats();

    if (root_key_ == INVALID_KEY) {
        AVLNodeData nd;
        nd.data = value;
        auto& oram = oram_for_depth(0);
        int leaf = oram.random_leaf();
        oram.set_leaf(key, leaf);
        oram.add_to_stash(key, leaf, nd.encode());
        root_key_ = key;
        root_leaf_ = leaf;
        pad_ops(0);
        end_stats();
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
        ++ops;
        auto& node = local_.back();

        if (key < cur_key) {
            if (node.avl.l_key == INVALID_KEY) {
                AVLNodeData new_nd;
                new_nd.data = value;
                int cd = d + 1;
                auto& co = oram_for_depth(cd);
                int nl = co.random_leaf();
                co.set_leaf(key, nl);
                node.avl.l_key = key;
                node.avl.l_leaf = nl;
                node.avl.l_height = 1;
                local_.push_back({key, nl, new_nd, cur_key, cd});
                break;
            }
            cur_key = node.avl.l_key;
            cur_leaf = node.avl.l_leaf;
        } else {
            if (node.avl.r_key == INVALID_KEY) {
                AVLNodeData new_nd;
                new_nd.data = value;
                int cd = d + 1;
                auto& co = oram_for_depth(cd);
                int nl = co.random_leaf();
                co.set_leaf(key, nl);
                node.avl.r_key = key;
                node.avl.r_leaf = nl;
                node.avl.r_height = 1;
                local_.push_back({key, nl, new_nd, cur_key, cd});
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
    pad_ops(ops);
    end_stats();
}

// ── Remove ──────────────────────────────────────────────────────────────────

void TeeAvlOmap::remove(int key) {
    begin_stats();

    if (root_key_ == INVALID_KEY) {
        pad_ops(0);
        end_stats();
        return;
    }

    local_.clear();
    int cur_key = root_key_;
    int cur_leaf = root_leaf_;
    int phase1_ops = 0;
    int target_idx = -1;
    bool two_child = false;

    for (int d = 0; d < max_height_; ++d) {
        if (cur_key == INVALID_KEY) break;
        int par = local_.empty() ? INVALID_KEY : local_.back().key;
        move_to_local(cur_key, cur_leaf, par, d);
        ++phase1_ops;

        int last_idx = static_cast<int>(local_.size()) - 1;

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
                    ++phase1_ops;
                    int si = static_cast<int>(local_.size()) - 1;
                    if (local_[si].avl.l_key == INVALID_KEY) break;
                    s_key = local_[si].avl.l_key;
                    s_leaf = local_[si].avl.l_leaf;
                }
            }
            break;
        } else if (key < local_[last_idx].key) {
            cur_key = local_[last_idx].avl.l_key;
            cur_leaf = local_[last_idx].avl.l_leaf;
        } else {
            cur_key = local_[last_idx].avl.r_key;
            cur_leaf = local_[last_idx].avl.r_leaf;
        }
    }

    if (target_idx >= 0 && target_idx < static_cast<int>(local_.size())) {
        if (two_child) {
            int succ_idx = static_cast<int>(local_.size()) - 1;
            int succ_key = local_[succ_idx].key;
            Bytes succ_data = local_[succ_idx].avl.data;
            int old_key = local_[target_idx].key;
            local_[target_idx].key = succ_key;
            local_[target_idx].avl.data = succ_data;

            if (local_[target_idx].parent_key != INVALID_KEY) {
                for (auto& p : local_) {
                    if (p.key != local_[target_idx].parent_key) continue;
                    if (p.avl.l_key == old_key) p.avl.l_key = succ_key;
                    else if (p.avl.r_key == old_key) p.avl.r_key = succ_key;
                    break;
                }
            }
            if (root_key_ == old_key) root_key_ = succ_key;
            for (auto& c : local_)
                if (c.parent_key == old_key) c.parent_key = succ_key;
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
            root_leaf_ = (child_key != INVALID_KEY) ? child_leaf : INVALID_LEAF;
        }
        local_.erase(local_.begin() + target_idx);
    }

    // Phase 2 rebalancing (simplified: re-read siblings as needed).
    std::vector<int> path_keys;
    path_keys.reserve(local_.size());
    for (auto& ln : local_) path_keys.push_back(ln.key);

    int phase2_ops = 0;
    for (int pi = static_cast<int>(path_keys.size()) - 1; pi >= 0; --pi) {
        int nd_key = path_keys[pi];
        int nd_idx = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == nd_key) { nd_idx = j; break; }
        if (nd_idx < 0) {
            lower_oram_.dummy_access();
            lower_oram_.dummy_access();
            phase2_ops += 2;
            continue;
        }

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
            continue;
        }

        int tall_key = (bal > 1) ? local_[nd_idx].avl.l_key
                                 : local_[nd_idx].avl.r_key;
        int tall_leaf = (bal > 1) ? local_[nd_idx].avl.l_leaf
                                  : local_[nd_idx].avl.r_leaf;
        bool tall_in = false;
        for (auto& n : local_)
            if (n.key == tall_key) { tall_in = true; break; }

        if (tall_key != INVALID_KEY && !tall_in)
            move_to_local(tall_key, tall_leaf, nd_key, nd_depth + 1);
        else
            oram_for_depth(nd_depth).dummy_access();
        ++phase2_ops;

        int tall_idx = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == tall_key) { tall_idx = j; break; }

        bool need_inner = false;
        int inner_key = INVALID_KEY, inner_leaf = INVALID_LEAF;
        int tall_d = nd_depth + 1;
        if (tall_idx >= 0) {
            tall_d = local_[tall_idx].depth;
            int tb = local_[tall_idx].avl.balance();
            if (bal > 1 && tb < 0) {
                inner_key = local_[tall_idx].avl.r_key;
                inner_leaf = local_[tall_idx].avl.r_leaf;
                need_inner = true;
            } else if (bal < -1 && tb > 0) {
                inner_key = local_[tall_idx].avl.l_key;
                inner_leaf = local_[tall_idx].avl.l_leaf;
                need_inner = true;
            }
        }

        if (need_inner && inner_key != INVALID_KEY) {
            bool in_local = false;
            for (auto& n : local_)
                if (n.key == inner_key) { in_local = true; break; }
            if (!in_local)
                move_to_local(inner_key, inner_leaf, tall_key, tall_d + 1);
            else
                oram_for_depth(nd_depth).dummy_access();
        } else {
            oram_for_depth(nd_depth).dummy_access();
        }
        ++phase2_ops;

        update_heights();
        int ni = -1;
        for (int j = 0; j < static_cast<int>(local_.size()); ++j)
            if (local_[j].key == nd_key) { ni = j; break; }
        if (ni >= 0) balance_node(ni);
        update_heights();
    }

    reassign_leaves();
    flush_local_to_stash();
    pad_ops(phase1_ops + phase2_ops);
    end_stats();
}

// ── Dummy ───────────────────────────────────────────────────────────────────

void TeeAvlOmap::dummy_access() {
    begin_stats();
    pad_ops(0);
    end_stats();
}

// ── Rebalancing ─────────────────────────────────────────────────────────────

void TeeAvlOmap::update_heights() {
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

std::tuple<int, int, int> TeeAvlOmap::rotate(int idx, bool left) {
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

    if (root_key_ == node.key) root_key_ = pivot.key;

    return {pivot.key, pivot.leaf, pivot.avl.height()};
}

std::tuple<int, int, int> TeeAvlOmap::balance_node(int idx) {
    auto& nd = local_[idx];
    int bal = nd.avl.balance();

    if (bal > 1) {
        for (int i = 0; i < static_cast<int>(local_.size()); ++i)
            if (local_[i].key == nd.avl.l_key && local_[i].avl.balance() < 0) {
                rotate(i, true);
                break;
            }
        return rotate(idx, false);
    }
    if (bal < -1) {
        for (int i = 0; i < static_cast<int>(local_.size()); ++i)
            if (local_[i].key == nd.avl.r_key && local_[i].avl.balance() > 0) {
                rotate(i, false);
                break;
            }
        return rotate(idx, true);
    }
    return {nd.key, nd.leaf, nd.avl.height()};
}

void TeeAvlOmap::rebalance() {
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        update_heights();
        balance_node(i);
    }
    update_heights();
}

}  // namespace tee
}  // namespace tiered_omap
