#include "tiered_omap/tee/tee_avl_omap.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace tiered_omap {
namespace tee {

static int node_oram_val_size(int user_val_size) {
    return AVL_HEADER_SIZE + user_val_size;
}

TeeAvlOmap::TeeAvlOmap(int capacity, int value_size, int bucket_size,
                        int split_depth)
    : split_depth_(split_depth), value_size_(value_size) {
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

// ── Init (not performance-critical; no obliviousness needed) ────────────────

void TeeAvlOmap::init(const std::vector<std::pair<int, Bytes>>& data) {
    if (data.empty()) {
        root_key_ = INVALID_KEY;
        root_leaf_ = INVALID_LEAF;
        return;
    }

    auto sorted = data;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });

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

    std::function<int(int)> fix_h = [&](int key) -> int {
        if (key == INVALID_KEY) return 0;
        auto& nd = nodes.at(key);
        nd.l_height = fix_h(nd.l_key);
        nd.r_height = fix_h(nd.r_key);
        return nd.height();
    };
    fix_h(root);

    // Pre-assign leaves locally (no pos_map_ needed).
    std::unordered_map<int, int> local_leaves;
    std::function<void(int, int)> assign = [&](int key, int depth) {
        if (key == INVALID_KEY) return;
        auto& oram = oram_for_depth(depth);
        local_leaves[key] = oram.random_leaf();
        auto& nd = nodes.at(key);
        assign(nd.l_key, depth + 1);
        assign(nd.r_key, depth + 1);
    };
    assign(root, 0);

    // Embed child leaf pointers from local_leaves.
    std::function<void(int, int)> fix_leaves = [&](int key, int depth) {
        if (key == INVALID_KEY) return;
        auto& nd = nodes.at(key);
        if (nd.l_key != INVALID_KEY)
            nd.l_leaf = local_leaves.at(nd.l_key);
        if (nd.r_key != INVALID_KEY)
            nd.r_leaf = local_leaves.at(nd.r_key);
        fix_leaves(nd.l_key, depth + 1);
        fix_leaves(nd.r_key, depth + 1);
    };
    fix_leaves(root, 0);

    if (split_depth_ > 0) {
        std::vector<std::pair<int, Bytes>> upper_data, lower_data;
        std::unordered_map<int, int> upper_leaves, lower_leaves;
        std::function<void(int, int)> partition = [&](int key, int depth) {
            if (key == INVALID_KEY) return;
            auto& nd = nodes.at(key);
            Bytes enc = nd.encode();
            if (depth < split_depth_) {
                upper_data.push_back({key, enc});
                upper_leaves[key] = local_leaves.at(key);
            } else {
                lower_data.push_back({key, enc});
                lower_leaves[key] = local_leaves.at(key);
            }
            partition(nd.l_key, depth + 1);
            partition(nd.r_key, depth + 1);
        };
        partition(root, 0);
        if (!upper_data.empty()) upper_oram_.init(upper_data, upper_leaves);
        if (!lower_data.empty()) lower_oram_.init(lower_data, lower_leaves);
    } else {
        std::vector<std::pair<int, Bytes>> all;
        for (auto& [k, nd] : nodes) all.push_back({k, nd.encode()});
        lower_oram_.init(all, local_leaves);
    }

    root_key_ = root;
    root_leaf_ = local_leaves.at(root);
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
    last_stats_.upper_node_accesses = split_depth_ > 0 ? upper_oram_.tracked_node_accesses() : 0;
    last_stats_.lower_node_accesses = lower_oram_.tracked_node_accesses();
}

// ── Doubly-oblivious ORAM step ──────────────────────────────────────────────
// Same cache-line and page access pattern regardless of is_real.
// Adds one entry to local_ (real or dummy with INVALID_KEY).

void TeeAvlOmap::oram_step(int depth, int key, int leaf,
                            int is_real, int parent_key) {
    auto& oram = oram_for_depth(depth);
    int use_leaf = o_select_i(is_real, leaf, oram.random_leaf());

    oram.read_path_to_stash(use_leaf);

    int use_key = o_select_i(is_real, key, -2);
    Block blk = oram.extract_from_stash(use_key);

    AVLNodeData nd = AVLNodeData::decode(blk.value);

    int entry_key = o_select_i(is_real, key, INVALID_KEY);
    int entry_leaf = o_select_i(is_real, blk.leaf, INVALID_LEAF);
    int entry_parent = o_select_i(is_real, parent_key, INVALID_KEY);

    local_.push_back({entry_key, entry_leaf, nd, entry_parent, depth});

    oram.evict_one_path(use_leaf);

    if (split_depth_ > 0 && depth < split_depth_)
        ++upper_ops_;
    else
        ++lower_ops_;
}

// Padding step: same access pattern as oram_step, but doesn't touch local_.
void TeeAvlOmap::pad_step(int depth) {
    auto& oram = oram_for_depth(depth);
    int leaf = oram.random_leaf();

    oram.read_path_to_stash(leaf);
    oram.extract_from_stash(-2);
    oram.evict_one_path(leaf);

    if (split_depth_ > 0 && depth < split_depth_)
        ++upper_ops_;
    else
        ++lower_ops_;
}

// Legacy move_to_local (used by insert/remove where obliviousness is
// applied at the caller level).
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

// ── Doubly-oblivious flush & reassign ───────────────────────────────────────

void TeeAvlOmap::flush_local_to_stash() {
    for (auto& node : local_) {
        // Only add real entries to stash (dummy entries have INVALID_KEY).
        // add_to_stash with INVALID_KEY is harmless — treated as dummy block.
        auto& oram = oram_for_depth(node.depth);
        oram.add_to_stash(node.key, node.leaf, node.avl.encode());
    }
    local_.clear();
}

void TeeAvlOmap::reassign_leaves() {
    // Phase 1: Assign new random leaves (stored in local_ entries only).
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& nd = local_[i];
        int is_real = 1 - o_equal(nd.key, INVALID_KEY);
        auto& oram = oram_for_depth(nd.depth);
        int new_leaf = oram.random_leaf();
        nd.leaf = o_select_i(is_real, new_leaf, nd.leaf);
    }

    // Phase 2: Update parent-child leaf pointers (full scan, no breaks).
    for (int i = static_cast<int>(local_.size()) - 1; i >= 0; --i) {
        auto& child = local_[i];
        int child_real = 1 - o_equal(child.key, INVALID_KEY);
        int has_parent = 1 - o_equal(child.parent_key, INVALID_KEY);
        int should_update = child_real & has_parent;

        for (auto& par : local_) {
            int par_real = 1 - o_equal(par.key, INVALID_KEY);
            int is_parent = should_update & par_real
                          & o_equal(par.key, child.parent_key);
            int is_left = is_parent & o_equal(par.avl.l_key, child.key);
            int is_right = is_parent & o_equal(par.avl.r_key, child.key);
            o_mov_i(is_left, par.avl.l_leaf, child.leaf);
            o_mov_i(is_right, par.avl.r_leaf, child.leaf);
        }
    }

    // Update root leaf (full scan).
    for (auto& nd : local_) {
        int is_root = o_equal(nd.key, root_key_)
                    & (1 - o_equal(nd.key, INVALID_KEY));
        o_mov_i(is_root, root_leaf_, nd.leaf);
    }
}

void TeeAvlOmap::pad_ops(int ops_done) {
    int budget = 3 * max_height_;
    int pad = std::max(0, budget - ops_done);
    if (split_depth_ > 0) {
        int eff_upper = std::min(split_depth_, budget);
        int upper_pad = std::max(0, eff_upper - upper_ops_);
        for (int i = 0; i < upper_pad; ++i)
            pad_step(0);
        int lower_pad = std::max(0, pad - upper_pad);
        for (int i = 0; i < lower_pad; ++i)
            pad_step(split_depth_);
    } else {
        for (int i = 0; i < pad; ++i)
            pad_step(0);
    }
}

// ── Doubly-oblivious Search ─────────────────────────────────────────────────

Bytes TeeAvlOmap::search(int key, const Bytes* update,
                         ValueTransform transform) {
    bool found_flag = false;
    Bytes result = search_or_dummy(key, true, update,
                                   std::move(transform), &found_flag);
    if (!found_flag) result.clear();
    return result;
}

Bytes TeeAvlOmap::search_or_dummy(int key, bool real, const Bytes* update,
                                   ValueTransform transform,
                                   bool* out_found) {
    begin_stats();
    local_.clear();

    Bytes result(value_size_, 0);

    // Obliviously select root or invalid start.
    int cur_key = o_select_i(real, root_key_, INVALID_KEY);
    int cur_leaf = o_select_i(real, root_leaf_, INVALID_LEAF);
    int found = 0;
    int found_idx = -1;

    for (int d = 0; d < max_height_; ++d) {
        int is_valid = 1 - o_equal(cur_key, INVALID_KEY);
        int is_searching = is_valid & (1 - found);

        int par_key = (d == 0) ? INVALID_KEY : local_[d - 1].key;
        par_key = o_select_i(is_searching, par_key, INVALID_KEY);

        oram_step(d, cur_key, cur_leaf, is_searching, par_key);

        auto& node = local_.back();

        // Oblivious match detection.
        int match = is_searching & o_equal(key, node.key);

        // Record result obliviously.
        o_mov_bytes(match, result, node.avl.data);
        if (update) {
            Bytes upd = pad_bytes(*update, value_size_);
            o_mov_bytes(match, node.avl.data, upd);
        }
        o_mov_i(match, found_idx, d);
        found |= match;

        // Oblivious child selection.
        int go_left = is_searching & (1 - match) & o_less(key, cur_key);
        int still_searching = is_searching & (1 - match);
        int next_key = o_select_i(go_left, node.avl.l_key, node.avl.r_key);
        int next_leaf = o_select_i(go_left, node.avl.l_leaf, node.avl.r_leaf);

        cur_key = o_select_i(still_searching, next_key, INVALID_KEY);
        cur_leaf = o_select_i(still_searching, next_leaf, INVALID_LEAF);
    }

    // Apply value transform (e.g., epoch update) before flush.
    // Always called to keep access pattern constant; write-back is
    // obliviously guarded by found.
    if (transform) {
        Bytes transformed = transform(result);
        Bytes padded = pad_bytes(transformed, value_size_);
        for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
            int is_match = found & o_equal(i, found_idx);
            o_mov_bytes(is_match, local_[i].avl.data, padded);
        }
    }

    reassign_leaves();
    flush_local_to_stash();
    pad_ops(max_height_);
    end_stats();
    // Obliviously zero result when not found (result stays value_size_ bytes).
    Bytes zeros(value_size_, 0);
    o_mov_bytes(1 - found, result, zeros);
    if (out_found) *out_found = (found != 0);
    return result;
}

// ── Doubly-oblivious Insert ─────────────────────────────────────────────────

void TeeAvlOmap::insert(int key, const Bytes& value) {
    begin_stats();
    local_.clear();
    local_.reserve(max_height_ + 2);

    // When key=INVALID_KEY this is a dummy insert (same ORAM pattern, no tree change).
    int real = 1 - o_equal(key, INVALID_KEY);
    int was_empty = real & o_equal(root_key_, INVALID_KEY);

    int cur_key = o_select_i(real, root_key_, INVALID_KEY);
    int cur_leaf = o_select_i(real, root_leaf_, INVALID_LEAF);
    int inserted = 0;
    int insert_parent = INVALID_KEY;
    int insert_depth = 0;
    int insert_leaf = INVALID_LEAF;

    for (int d = 0; d < max_height_; ++d) {
        int is_valid = 1 - o_equal(cur_key, INVALID_KEY);
        int is_searching = is_valid & (1 - inserted);

        int par_key = local_.empty() ? INVALID_KEY : local_.back().key;
        par_key = o_select_i(is_searching, par_key, INVALID_KEY);

        oram_step(d, cur_key, cur_leaf, is_searching, par_key);

        int step_idx = static_cast<int>(local_.size()) - 1;

        int go_left = is_searching & o_less(key, cur_key);
        int go_right = is_searching & (1 - go_left);

        int left_child = local_[step_idx].avl.l_key;
        int right_child = local_[step_idx].avl.r_key;
        int child_key = o_select_i(go_left, left_child, right_child);
        int child_empty = o_equal(child_key, INVALID_KEY);
        int should_insert = is_searching & child_empty;

        int cd = d + 1;
        auto& co = oram_for_depth(cd);
        int nl = co.random_leaf();

        int one = 1;
        o_mov_i(should_insert & go_left, local_[step_idx].avl.l_key, key);
        o_mov_i(should_insert & go_left, local_[step_idx].avl.l_leaf, nl);
        o_mov_i(should_insert & go_left, local_[step_idx].avl.l_height, one);

        o_mov_i(should_insert & go_right, local_[step_idx].avl.r_key, key);
        o_mov_i(should_insert & go_right, local_[step_idx].avl.r_leaf, nl);
        o_mov_i(should_insert & go_right, local_[step_idx].avl.r_height, one);

        o_mov_i(should_insert, insert_parent, cur_key);
        o_mov_i(should_insert, insert_depth, cd);
        o_mov_i(should_insert, insert_leaf, nl);

        inserted |= should_insert;

        int still_searching = is_searching & (1 - inserted);
        int next_key = o_select_i(go_left,
            local_[step_idx].avl.l_key, local_[step_idx].avl.r_key);
        int next_leaf = o_select_i(go_left,
            local_[step_idx].avl.l_leaf, local_[step_idx].avl.r_leaf);
        cur_key = o_select_i(still_searching, next_key, INVALID_KEY);
        cur_leaf = o_select_i(still_searching, next_leaf, INVALID_LEAF);
    }

    // For empty tree: new node becomes root at depth 0.
    auto& root_oram = oram_for_depth(0);
    int empty_leaf = root_oram.random_leaf();
    o_mov_i(was_empty, insert_leaf, empty_leaf);
    o_mov_i(was_empty, insert_depth, 0);

    int actually_inserted = inserted | was_empty;

    // Always push one entry (real or dummy) to keep local_ size constant.
    AVLNodeData new_nd;
    new_nd.data = pad_bytes(value, value_size_);
    int nk = o_select_i(actually_inserted, key, INVALID_KEY);
    int nl = o_select_i(actually_inserted, insert_leaf, INVALID_LEAF);
    int np = o_select_i(actually_inserted, insert_parent, INVALID_KEY);
    int nd = o_select_i(actually_inserted, insert_depth, 0);
    local_.push_back({nk, nl, new_nd, np, nd});

    // Set root before reassign_leaves so it can update root_leaf_.
    o_mov_i(was_empty, root_key_, key);

    update_heights();
    rebalance();
    reassign_leaves();
    flush_local_to_stash();
    pad_ops(max_height_);
    end_stats();
}

// ── Doubly-oblivious Remove ─────────────────────────────────────────────────
// Fixed ORAM budget: 2*max_height_ real oram_steps + padding to 3*max_height_
// (same total as search/insert). Rebalancing uses only nodes already in
// local_, without extra ORAM loads; the tree may be transiently slightly
// unbalanced, which subsequent inserts will fix.

void TeeAvlOmap::remove(int key) {
    begin_stats();
    local_.clear();
    local_.reserve(2 * max_height_ + 4);

    // When tree is empty or key=INVALID_KEY, all phases still execute
    // with dummy ORAM steps (same access pattern).
    int real = (1 - o_equal(root_key_, INVALID_KEY))
             & (1 - o_equal(key, INVALID_KEY));

    // ── Phase 1: Find target (fixed max_height_ oram_steps) ────────────
    int cur_key = o_select_i(real, root_key_, INVALID_KEY);
    int cur_leaf = o_select_i(real, root_leaf_, INVALID_LEAF);
    int found = 0;
    int target_idx = -1;
    int target_depth_val = 0;

    for (int d = 0; d < max_height_; ++d) {
        int is_valid = 1 - o_equal(cur_key, INVALID_KEY);
        int is_searching = is_valid & (1 - found);

        int par_key = local_.empty() ? INVALID_KEY : local_.back().key;
        par_key = o_select_i(is_searching, par_key, INVALID_KEY);

        oram_step(d, cur_key, cur_leaf, is_searching, par_key);

        int li = static_cast<int>(local_.size()) - 1;
        int match = is_searching & o_equal(key, local_[li].key);
        target_idx = o_select_i(match, li, target_idx);
        target_depth_val = o_select_i(match, d, target_depth_val);
        found |= match;

        int go_left = is_searching & (1 - match) & o_less(key, cur_key);
        int still = is_searching & (1 - match);
        int nk = o_select_i(go_left,
            local_[li].avl.l_key, local_[li].avl.r_key);
        int nl = o_select_i(go_left,
            local_[li].avl.l_leaf, local_[li].avl.r_leaf);
        cur_key = o_select_i(still, nk, INVALID_KEY);
        cur_leaf = o_select_i(still, nl, INVALID_LEAF);
    }

    // ── Phase 2: Find successor (fixed max_height_ oram_steps) ─────────
    // Read target's children obliviously (full scan).
    int tr_key = INVALID_KEY, tr_leaf = INVALID_LEAF, tl_key = INVALID_KEY;
    int n_local = static_cast<int>(local_.size());
    for (int i = 0; i < n_local; ++i) {
        int is_t = found & o_equal(i, target_idx);
        o_mov_i(is_t, tr_key, local_[i].avl.r_key);
        o_mov_i(is_t, tr_leaf, local_[i].avl.r_leaf);
        o_mov_i(is_t, tl_key, local_[i].avl.l_key);
    }

    int has_two = found
               & (1 - o_equal(tl_key, INVALID_KEY))
               & (1 - o_equal(tr_key, INVALID_KEY));

    int sk = o_select_i(has_two, tr_key, INVALID_KEY);
    int sl = o_select_i(has_two, tr_leaf, INVALID_LEAF);
    int sf = 0;
    int succ_idx = -1;

    for (int sd = 0; sd < max_height_; ++sd) {
        int vs = (1 - sf) & (1 - o_equal(sk, INVALID_KEY));

        // First step's parent is the target itself; subsequent steps
        // use the previous Phase-2 entry.
        int ps = (sd == 0) ? key : local_.back().key;
        ps = o_select_i(vs, ps, INVALID_KEY);

        int ds = target_depth_val + 1 + sd;
        oram_step(ds, sk, sl, vs, ps);

        int si = static_cast<int>(local_.size()) - 1;
        int le = o_equal(local_[si].avl.l_key, INVALID_KEY);
        int is_s = vs & le & (1 - sf);
        succ_idx = o_select_i(is_s, si, succ_idx);
        sf |= is_s;

        int ss = vs & (1 - sf);
        sk = o_select_i(ss, local_[si].avl.l_key, INVALID_KEY);
        sl = o_select_i(ss, local_[si].avl.l_leaf, INVALID_LEAF);
    }

    // ── Phase 3: In-enclave pointer surgery (no ORAM access) ───────────
    int del_idx = o_select_i(has_two, succ_idx, target_idx);

    // 3a: For two-child case, copy successor's key+data to target.
    int sk_val = INVALID_KEY;
    Bytes s_data(value_size_, 0);
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int is_s = has_two & o_equal(i, succ_idx);
        o_mov_i(is_s, sk_val, local_[i].key);
        o_mov_bytes(is_s, s_data, local_[i].avl.data);
    }

    int old_tk = INVALID_KEY;
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int is_t = has_two & o_equal(i, target_idx);
        o_mov_i(is_t, old_tk, local_[i].key);
        o_mov_i(is_t, local_[i].key, sk_val);
        o_mov_bytes(is_t, local_[i].avl.data, s_data);
    }

    // Fix parent child pointers: old_tk → sk_val.
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int active = has_two & (1 - o_equal(local_[i].key, INVALID_KEY));
        int fl = active & o_equal(local_[i].avl.l_key, old_tk);
        int fr = active & o_equal(local_[i].avl.r_key, old_tk);
        o_mov_i(fl, local_[i].avl.l_key, sk_val);
        o_mov_i(fr, local_[i].avl.r_key, sk_val);
    }
    o_mov_i(has_two & o_equal(root_key_, old_tk), root_key_, sk_val);

    // Fix children's parent_key.
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int fix = has_two & o_equal(local_[i].parent_key, old_tk);
        o_mov_i(fix, local_[i].parent_key, sk_val);
    }

    // 3b: Unlink the actually-deleted node (del_idx).
    int dk = INVALID_KEY, dp = INVALID_KEY;
    int dl_k = INVALID_KEY, dl_l = INVALID_LEAF, dl_h = 0;
    int dr_k = INVALID_KEY, dr_l = INVALID_LEAF, dr_h = 0;
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int is_d = found & o_equal(i, del_idx);
        o_mov_i(is_d, dk, local_[i].key);
        o_mov_i(is_d, dp, local_[i].parent_key);
        o_mov_i(is_d, dl_k, local_[i].avl.l_key);
        o_mov_i(is_d, dl_l, local_[i].avl.l_leaf);
        o_mov_i(is_d, dl_h, local_[i].avl.l_height);
        o_mov_i(is_d, dr_k, local_[i].avl.r_key);
        o_mov_i(is_d, dr_l, local_[i].avl.r_leaf);
        o_mov_i(is_d, dr_h, local_[i].avl.r_height);
    }

    int has_l = 1 - o_equal(dl_k, INVALID_KEY);
    int ck = o_select_i(has_l, dl_k, dr_k);
    int cl = o_select_i(has_l, dl_l, dr_l);
    int ch = o_select_i(has_l, dl_h, dr_h);
    ch = o_select_i(o_equal(ck, INVALID_KEY), 0, ch);

    // Parent now points to the replacement child.
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int is_p = found
                 & (1 - o_equal(dp, INVALID_KEY))
                 & o_equal(local_[i].key, dp)
                 & (1 - o_equal(local_[i].key, INVALID_KEY));
        int fl = is_p & o_equal(local_[i].avl.l_key, dk);
        int fr = is_p & o_equal(local_[i].avl.r_key, dk);
        o_mov_i(fl, local_[i].avl.l_key, ck);
        o_mov_i(fl, local_[i].avl.l_leaf, cl);
        o_mov_i(fl, local_[i].avl.l_height, ch);
        o_mov_i(fr, local_[i].avl.r_key, ck);
        o_mov_i(fr, local_[i].avl.r_leaf, cl);
        o_mov_i(fr, local_[i].avl.r_height, ch);
    }

    // If deleted node was the root.
    int d_root = found & o_equal(dp, INVALID_KEY);
    o_mov_i(d_root, root_key_, ck);
    o_mov_i(d_root, root_leaf_,
            o_select_i(o_equal(ck, INVALID_KEY), INVALID_LEAF, cl));

    // Mark deleted node as INVALID_KEY.
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int is_d = found & o_equal(i, del_idx);
        int inv = INVALID_KEY;
        o_mov_i(is_d, local_[i].key, inv);
    }

    // Fix replacement child's parent_key (only the actual child ck, not
    // other nodes that happen to share parent_key == dk after renaming).
    for (int i = 0; i < static_cast<int>(local_.size()); ++i) {
        int fix = found
                & (1 - o_equal(ck, INVALID_KEY))
                & (1 - o_equal(local_[i].key, INVALID_KEY))
                & o_equal(local_[i].key, ck);
        o_mov_i(fix, local_[i].parent_key, dp);
    }

    // ── Phase 4: In-enclave rebalancing (no ORAM access) ───────────────
    // Uses only nodes already in local_. Rotations needing nodes not in
    // local_ are gracefully skipped; subsequent inserts will rebalance.
    update_heights();
    rebalance();

    reassign_leaves();
    flush_local_to_stash();
    pad_ops(2 * max_height_);
    end_stats();
}

// ── Dummy ───────────────────────────────────────────────────────────────────

void TeeAvlOmap::dummy_access() {
    search_or_dummy(INVALID_KEY, false, nullptr);
}

// ── Rebalancing (in-enclave local_ operations) ──────────────────────────────

// ── Fully oblivious AVL rebalancing ─────────────────────────────────────────
// All operations use fixed iteration counts and oblivious conditional moves.
// No data-dependent branches, continue, break, or early returns.

void TeeAvlOmap::update_heights() {
    int n = static_cast<int>(local_.size());
    for (int i = n - 1; i >= 0; --i) {
        auto& nd = local_[i];
        int nd_real = 1 - o_equal(nd.key, INVALID_KEY);
        for (int j = 0; j < n; ++j) {
            auto& child = local_[j];
            int c_real = 1 - o_equal(child.key, INVALID_KEY);
            int active = nd_real & c_real;
            int ch = 1 + o_max(child.avl.l_height, child.avl.r_height);
            int is_lc = active & o_equal(child.key, nd.avl.l_key);
            int is_rc = active & o_equal(child.key, nd.avl.r_key);
            o_mov_i(is_lc, nd.avl.l_height, ch);
            o_mov_i(is_rc, nd.avl.r_height, ch);
        }
    }
}

// Oblivious conditional rotation.
// dir: 1 = right rotation (left-heavy), 0 = left rotation (right-heavy).
// active: 1 = actually perform, 0 = no-op (same code path executed).
std::tuple<int, int, int> TeeAvlOmap::rotate(int idx, bool /*unused*/) {
    // This legacy signature is no longer called; see rotate_cond below.
    return {local_[idx].key, local_[idx].leaf, local_[idx].avl.height()};
}

void TeeAvlOmap::rotate_cond(int idx, int active, int dir) {
    auto& node = local_[idx];
    int n = static_cast<int>(local_.size());

    // Pivot is: right child for left rotation (dir=0), left child for right (dir=1).
    int pivot_key_target = o_select_i(dir, node.avl.l_key, node.avl.r_key);

    // ── Oblivious gather: extract pivot data via full scan ──────────────
    int pivot_idx = 0;
    int pivot_found = 0;
    int p_key = INVALID_KEY, p_leaf = INVALID_LEAF, p_parent = INVALID_KEY;
    int p_lk = INVALID_KEY, p_ll = INVALID_LEAF, p_lh = 0;
    int p_rk = INVALID_KEY, p_rl = INVALID_LEAF, p_rh = 0;
    for (int i = 0; i < n; ++i) {
        int match = o_equal(local_[i].key, pivot_key_target)
                  & (1 - o_equal(local_[i].key, INVALID_KEY))
                  & (1 - o_equal(pivot_key_target, INVALID_KEY));
        o_mov_i(match, pivot_idx, i);
        o_mov_i(match, p_key, local_[i].key);
        o_mov_i(match, p_leaf, local_[i].leaf);
        o_mov_i(match, p_parent, local_[i].parent_key);
        o_mov_i(match, p_lk, local_[i].avl.l_key);
        o_mov_i(match, p_ll, local_[i].avl.l_leaf);
        o_mov_i(match, p_lh, local_[i].avl.l_height);
        o_mov_i(match, p_rk, local_[i].avl.r_key);
        o_mov_i(match, p_rl, local_[i].avl.r_leaf);
        o_mov_i(match, p_rh, local_[i].avl.r_height);
        pivot_found |= match;
    }
    int act = active & pivot_found;

    // Transferred subtree: pivot's inner child.
    int trans_key  = o_select_i(dir, p_rk, p_lk);
    int trans_leaf = o_select_i(dir, p_rl, p_ll);
    int trans_h    = o_select_i(dir, p_rh, p_lh);

    // Step 1: node's inner child ← transferred subtree.
    int is_left_rot = act & (1 - dir);
    int is_right_rot = act & dir;
    o_mov_i(is_left_rot, node.avl.r_key, trans_key);
    o_mov_i(is_left_rot, node.avl.r_leaf, trans_leaf);
    o_mov_i(is_left_rot, node.avl.r_height, trans_h);
    o_mov_i(is_right_rot, node.avl.l_key, trans_key);
    o_mov_i(is_right_rot, node.avl.l_leaf, trans_leaf);
    o_mov_i(is_right_rot, node.avl.l_height, trans_h);

    // Recalculate node height after child pointer change.
    int new_node_h = 1 + o_max(
        o_select_i(dir, trans_h, node.avl.l_height),
        o_select_i(dir, node.avl.r_height, trans_h));

    // Step 2: pivot's inner child ← node (update gathered pivot data).
    o_mov_i(is_left_rot, p_lk, node.key);
    o_mov_i(is_left_rot, p_ll, node.leaf);
    o_mov_i(is_left_rot, p_lh, new_node_h);
    o_mov_i(is_right_rot, p_rk, node.key);
    o_mov_i(is_right_rot, p_rl, node.leaf);
    o_mov_i(is_right_rot, p_rh, new_node_h);

    // Save original parent before updating parent_key fields.
    int orig_node_parent = node.parent_key;

    // Step 3: Fix parent_key fields (on gathered data).
    o_mov_i(act, p_parent, orig_node_parent);
    o_mov_i(act, node.parent_key, p_key);

    // ── Oblivious scatter: write pivot data back + fix parent pointers ──
    int new_p_h = 1 + o_max(p_lh, p_rh);
    for (int i = 0; i < n; ++i) {
        // Scatter pivot fields back to matching entry.
        int is_pivot = act & o_equal(i, pivot_idx);
        o_mov_i(is_pivot, local_[i].parent_key, p_parent);
        o_mov_i(is_pivot, local_[i].avl.l_key, p_lk);
        o_mov_i(is_pivot, local_[i].avl.l_leaf, p_ll);
        o_mov_i(is_pivot, local_[i].avl.l_height, p_lh);
        o_mov_i(is_pivot, local_[i].avl.r_key, p_rk);
        o_mov_i(is_pivot, local_[i].avl.r_leaf, p_rl);
        o_mov_i(is_pivot, local_[i].avl.r_height, p_rh);

        // Update parent's child pointer: node → pivot.
        // Use orig_node_parent (before the parent_key swap).
        int is_par = act
                   & o_equal(local_[i].key, orig_node_parent)
                   & (1 - o_equal(local_[i].key, INVALID_KEY));
        int fl = is_par & o_equal(local_[i].avl.l_key, node.key);
        int fr = is_par & o_equal(local_[i].avl.r_key, node.key);
        o_mov_i(fl, local_[i].avl.l_key, p_key);
        o_mov_i(fl, local_[i].avl.l_leaf, p_leaf);
        o_mov_i(fl, local_[i].avl.l_height, new_p_h);
        o_mov_i(fr, local_[i].avl.r_key, p_key);
        o_mov_i(fr, local_[i].avl.r_leaf, p_leaf);
        o_mov_i(fr, local_[i].avl.r_height, new_p_h);

        // Transferred child's parent_key → node.
        int fix_trans = act
                      & (1 - o_equal(trans_key, INVALID_KEY))
                      & (1 - o_equal(local_[i].key, INVALID_KEY))
                      & o_equal(local_[i].key, trans_key);
        o_mov_i(fix_trans, local_[i].parent_key, node.key);
    }

    // Update root.
    int fix_root = act & o_equal(root_key_, node.key);
    o_mov_i(fix_root, root_key_, p_key);
}

std::tuple<int, int, int> TeeAvlOmap::balance_node(int idx) {
    auto& nd = local_[idx];
    int is_real = 1 - o_equal(nd.key, INVALID_KEY);
    int bal = nd.avl.l_height - nd.avl.r_height;
    int n = static_cast<int>(local_.size());

    int need_right = is_real & o_less(1, bal);
    int need_left = is_real & o_less(bal, -1);

    // Oblivious gather: find child indices AND their balance factors
    // via full scan (no secret-indexed access).
    int lc_idx = 0, rc_idx = 0;
    int lc_lh = 0, lc_rh = 0, rc_lh = 0, rc_rh = 0;
    for (int i = 0; i < n; ++i) {
        int c_real = 1 - o_equal(local_[i].key, INVALID_KEY);
        int is_lc = c_real & o_equal(local_[i].key, nd.avl.l_key);
        int is_rc = c_real & o_equal(local_[i].key, nd.avl.r_key);
        o_mov_i(is_lc, lc_idx, i);
        o_mov_i(is_lc, lc_lh, local_[i].avl.l_height);
        o_mov_i(is_lc, lc_rh, local_[i].avl.r_height);
        o_mov_i(is_rc, rc_idx, i);
        o_mov_i(is_rc, rc_lh, local_[i].avl.l_height);
        o_mov_i(is_rc, rc_rh, local_[i].avl.r_height);
    }

    int lc_bal = lc_lh - lc_rh;
    int need_lr = need_right & o_less(lc_bal, 0);
    int rc_bal = rc_lh - rc_rh;
    int need_rl = need_left & o_less(0, rc_bal);

    rotate_cond(lc_idx, need_lr, 0);
    rotate_cond(rc_idx, need_rl, 1);
    rotate_cond(idx, need_right, 1);
    rotate_cond(idx, need_left, 0);

    return {nd.key, nd.leaf, nd.avl.height()};
}

void TeeAvlOmap::rebalance() {
    int n = static_cast<int>(local_.size());
    for (int i = n - 1; i >= 0; --i) {
        update_heights();
        balance_node(i);
    }
    update_heights();
}

}  // namespace tee
}  // namespace tiered_omap
