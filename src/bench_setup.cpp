#include "tiered_omap/bench_setup.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include "tiered_omap/oram/disk_binary_tree_storage.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace tiered_omap {
namespace bench_setup {

// ── Helpers ─────────────────────────────────────────────────────────────

static void si(Bytes& b, int v) {
    size_t p = b.size(); b.resize(p + 4); std::memcpy(b.data() + p, &v, 4);
}
static int di(const uint8_t*& p) {
    int v; std::memcpy(&v, p, 4); p += 4; return v;
}

static std::vector<std::pair<int, Bytes>> make_data(int N, int value_size) {
    std::vector<std::pair<int, Bytes>> d;
    d.reserve(N);
    for (int i = 0; i < N; ++i) {
        Bytes v(value_size, 0);
        std::memcpy(v.data(), &i, std::min(sizeof(int), (size_t)value_size));
        d.emplace_back(i, std::move(v));
    }
    return d;
}

static int register_storage(StorageServer::ClientState& stores,
                            std::unique_ptr<StorageInterface> s) {
    if (!s) return -1;
    return stores.register_store(std::move(s));
}

static StorageCreator make_data_storage_creator(
    const StorageServer::ClientState& stores) {
    if (stores.disk_store_dir.empty())
        return nullptr;
    return [dir = stores.disk_store_dir](int num_data, int bucket_size)
        -> std::unique_ptr<StorageInterface> {
        return std::make_unique<DiskBinaryTreeStorage>(
            num_data, bucket_size, dir);
    };
}

// Patch a store_id at a given byte offset in the blob.
static void patch_sid(Bytes& blob, int offset, int sid) {
    std::memcpy(blob.data() + offset, &sid, 4);
}

static void write_bytes_file(const std::filesystem::path& path,
                             const Bytes& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("write_bytes_file: open failed");
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out)
            throw std::runtime_error("write_bytes_file: write failed");
    }
}

static Bytes read_bytes_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("read_bytes_file: open failed");
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    Bytes data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!in)
            throw std::runtime_error("read_bytes_file: read failed");
    }
    return data;
}

static std::filesystem::path data_cache_dir(
    const StorageServer::ClientState& stores, int N, int bucket_size,
    int value_size) {
    if (stores.data_oram_cache_dir.empty())
        return {};
    std::filesystem::path dir(stores.data_oram_cache_dir);
    dir /= "pathoram-v1-N" + std::to_string(N)
         + "-bs" + std::to_string(bucket_size)
         + "-vs" + std::to_string(value_size);
    return dir;
}

static Bytes setup_data_oram_blob(
    StorageServer::ClientState& stores, int N, int bucket_size,
    int value_size) {
    auto cache_dir = data_cache_dir(stores, N, bucket_size, value_size);
    if (!cache_dir.empty()) {
        auto blob_path = cache_dir / "path_oram.bin";
        auto meta_path = cache_dir / "store.meta";
        auto data_path = cache_dir / "store.bin";
        if (std::filesystem::exists(blob_path) &&
            std::filesystem::exists(meta_path) &&
            std::filesystem::exists(data_path)) {
            try {
                Bytes data_blob = read_bytes_file(blob_path);
                auto store = DiskBinaryTreeStorage::load_file_snapshot(
                    meta_path.string(), data_path.string(),
                    stores.disk_store_dir);
                int sid = register_storage(stores, std::move(store));
                patch_sid(data_blob, 0, sid);
                std::cerr << "[bench_setup] data_oram cache hit: "
                          << cache_dir << "\n";
                return data_blob;
            } catch (const std::exception& e) {
                std::cerr << "[bench_setup] data_oram cache load failed: "
                          << e.what() << "\n";
            }
        }
    }

    PathORAM data_oram(N, bucket_size, 7, make_data_storage_creator(stores));
    data_oram.init_sequential(N, value_size);
    Bytes data_blob = data_oram.export_state(0);
    auto storage = data_oram.detach_storage();

    if (!cache_dir.empty()) {
        if (auto* disk = dynamic_cast<DiskBinaryTreeStorage*>(storage.get())) {
            std::filesystem::path tmp =
                cache_dir.parent_path() /
                (cache_dir.filename().string() + ".tmp." +
                 std::to_string(::getpid()));
            try {
                if (!std::filesystem::exists(cache_dir)) {
                    std::filesystem::create_directories(tmp);
                    write_bytes_file(tmp / "path_oram.bin", data_blob);
                    disk->save_file_snapshot(
                        (tmp / "store.meta").string(),
                        (tmp / "store.bin").string());
                    std::filesystem::rename(tmp, cache_dir);
                    std::cerr << "[bench_setup] data_oram cache saved: "
                              << cache_dir << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[bench_setup] data_oram cache save failed: "
                          << e.what() << "\n";
                std::error_code ec;
                std::filesystem::remove_all(tmp, ec);
            }
        }
    }

    int sid = register_storage(stores, std::move(storage));
    patch_sid(data_blob, 0, sid);
    return data_blob;
}

// Walk ORAMs in a standalone OMAP: detach, register, patch.
// Returns the number of bytes consumed from omap_blob for offset tracking.
static void detach_register_patch_avl(
    StorageServer::ClientState& stores, AVLOmap& avl,
    Bytes& blob, int base_offset) {
    // AVL export layout:
    //   capacity(4) max_height(4) root_key(4) root_leaf(4) split_depth(4) ods_mode(4) = 24 bytes header
    //   b0_len(4) + b0_data[b0_len]  (PathORAM blob, store_id at byte 0 of b0_data)
    //   if split: b1_len(4) + b1_data[b1_len]

    int hdr = 24; // 6 ints
    int b0_len;
    std::memcpy(&b0_len, blob.data() + base_offset + hdr, 4);
    int b0_sid_offset = base_offset + hdr + 4; // first byte of PathORAM blob

    auto s0 = avl.oram().detach_storage();
    int sid0 = register_storage(stores, std::move(s0));
    patch_sid(blob, b0_sid_offset, sid0);

    int split_depth;
    std::memcpy(&split_depth, blob.data() + base_offset + 16, 4);
    if (split_depth > 0) {
        int b1_offset = base_offset + hdr + 4 + b0_len;
        int b1_len;
        std::memcpy(&b1_len, blob.data() + b1_offset, 4);
        int b1_sid_offset = b1_offset + 4;

        auto s1 = avl.upper_oram().detach_storage();
        int sid1 = register_storage(stores, std::move(s1));
        patch_sid(blob, b1_sid_offset, sid1);
    }
}

static void detach_register_patch_bplus(
    StorageServer::ClientState& stores, BPlusOmap& bp,
    Bytes& blob, int base_offset) {
    // BPlus layout: root_id(4) root_leaf(4) next_block_id(4) order(4)
    //   max_height(4) ods_mode(4) split_depth(4) index_mode(4) = 32 bytes
    //   b0_len(4) + b0_data[b0_len]
    //   if split: b1_len(4) + b1_data[b1_len]
    int hdr = 32;
    int b0_len;
    std::memcpy(&b0_len, blob.data() + base_offset + hdr, 4);
    int b0_sid_offset = base_offset + hdr + 4;

    auto s0 = bp.oram().detach_storage();
    int sid0 = register_storage(stores, std::move(s0));
    patch_sid(blob, b0_sid_offset, sid0);

    int split_depth;
    std::memcpy(&split_depth, blob.data() + base_offset + 24, 4);
    if (split_depth > 0) {
        int b1_offset = base_offset + hdr + 4 + b0_len;
        int b1_sid_offset = b1_offset + 4;

        auto s1 = bp.upper_oram().detach_storage();
        int sid1 = register_storage(stores, std::move(s1));
        patch_sid(blob, b1_sid_offset, sid1);
    }
}

static void detach_register_patch_daost(
    StorageServer::ClientState& stores, DaOstOmap& da,
    Bytes& blob, int base_offset) {
    // DaOstOmap layout:
    //   capacity(4) num_positions(4) tree_height_bound(4) ods_budget(4)
    //   tree_type(4) bucket_size(4) bplus_order(4) hash_seed(8) = 36 bytes
    //   root_cache_size(4) + root_cache data (3 ints each)
    //   da_blob_len(4) + da_blob (DAOram)
    //   ods_blob_len(4) + ods_blob (AVL or BPlus)

    const uint8_t* scan = blob.data() + base_offset;
    scan += 7 * 4 + 8; // skip header (7 ints + 8 bytes hash_seed)
    int rc_sz;
    std::memcpy(&rc_sz, scan, 4); scan += 4;
    scan += rc_sz * 3 * 4; // skip root_cache entries

    int da_len;
    std::memcpy(&da_len, scan, 4); scan += 4;
    int da_data_start = static_cast<int>(scan - blob.data());

    // DAOram blob layout:
    //   store_id(4) level(4) leaf_range(4)
    //   num_data(4) bucket_size(4) stash_max(4) num_ic(4) ic_max(4) on_chip_mem(4) block_size(4)
    //   prf_seed(8)
    //   num_pos_levels(4) level_sizes[num_pos_levels](4 each)
    //   on_chip_size(4) + on_chip entries (each: len(4) + data[len])
    //   pos_map PathORAM blobs (each: len(4) + PathORAM_blob[len], store_id at byte 0 of PathORAM_blob)
    //   stash_size(4) + stash entries ...
    //   pending_leaves_size(4) + ...

    // 1) Patch data ORAM store_id (at byte 0 of DA blob)
    auto s_da = da.daoram().detach_storage();
    int sid_da = register_storage(stores, std::move(s_da));
    patch_sid(blob, da_data_start, sid_da);

    // 2) Walk into the DA blob to find and patch each pos_map PathORAM store_id
    const uint8_t* da_scan = blob.data() + da_data_start;
    da_scan += 4 + 4 + 4; // skip store_id, level, leaf_range
    da_scan += 7 * 4;     // skip num_data..block_size_bytes (7 ints)
    da_scan += 8;          // skip prf_seed

    int num_pos_levels;
    std::memcpy(&num_pos_levels, da_scan, 4); da_scan += 4;
    da_scan += num_pos_levels * 4; // skip level_sizes

    int oc_sz;
    std::memcpy(&oc_sz, da_scan, 4); da_scan += 4;
    for (int i = 0; i < oc_sz; ++i) {
        int cb_len;
        std::memcpy(&cb_len, da_scan, 4); da_scan += 4 + cb_len;
    }

    // Now at the pos_map PathORAM blobs
    for (int i = 0; i < num_pos_levels; ++i) {
        int pm_len;
        std::memcpy(&pm_len, da_scan, 4); da_scan += 4;
        int pm_sid_offset = static_cast<int>(da_scan - blob.data());

        auto pm_storage = da.daoram().pos_map_oram(i).detach_storage();
        int pm_sid = register_storage(stores, std::move(pm_storage));
        patch_sid(blob, pm_sid_offset, pm_sid);

        da_scan += pm_len;
    }

    scan += da_len; // skip past the DA blob

    // 3) Patch ODS OMAP
    int ods_off = static_cast<int>(scan - blob.data());
    int ods_len;
    std::memcpy(&ods_len, scan, 4);

    if (da.tree_type() == OdsTreeType::AVL) {
        detach_register_patch_avl(stores, da.avl_ods(), blob, ods_off + 4);
    } else {
        detach_register_patch_bplus(stores, da.bplus_ods(), blob, ods_off + 4);
    }
}

static void detach_register_patch_omap(
    StorageServer::ClientState& stores, OmapInterface* omap,
    OmapBackend be, Bytes& blob, int base_offset) {
    switch (be) {
    case OmapBackend::AVL:
        detach_register_patch_avl(stores, *static_cast<AVLOmap*>(omap), blob, base_offset);
        break;
    case OmapBackend::BPlus:
        detach_register_patch_bplus(stores, *static_cast<BPlusOmap*>(omap), blob, base_offset);
        break;
    case OmapBackend::DaAvl:
    case OmapBackend::DaBplus:
        detach_register_patch_daost(stores, *static_cast<DaOstOmap*>(omap), blob, base_offset);
        break;
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Server-side setup
// ═══════════════════════════════════════════════════════════════════════

static std::unique_ptr<OmapInterface> make_omap(OmapBackend be, int N, int bs) {
    switch (be) {
    case OmapBackend::AVL:    return std::make_unique<AVLOmap>(N, bs);
    case OmapBackend::BPlus:  return std::make_unique<BPlusOmap>(N, 8, bs);
    case OmapBackend::DaAvl:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::AVL, 0, bs);
    case OmapBackend::DaBplus:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::BPlus, 0, bs);
    }
    return nullptr;
}

static std::unique_ptr<OmapInterface> make_tiered_index_omap(
    OmapBackend be, int cap, bool is_cold, const TieredOMapConfig& cfg,
    int hot_ref_count) {
    int bs = cfg.bucket_size;
    switch (be) {
    case OmapBackend::BPlus: {
        if (is_cold && cfg.use_split_oram && hot_ref_count > 0) {
            int ord = cfg.bplus_order;
            int split_depth = std::max(1,
                static_cast<int>(std::floor(
                    std::log(std::max(hot_ref_count, 2)) /
                    std::log(std::max(ord, 2)))));
            int upper_cap = static_cast<int>(
                (std::pow(ord, split_depth) - 1) /
                std::max(ord - 1, 1));
            upper_cap = std::max(upper_cap, hot_ref_count);
            auto bp = std::make_unique<BPlusOmap>(
                cap, cfg.bplus_order, bs, split_depth, upper_cap);
            bp->set_index_mode(true);
            return bp;
        }
        auto bp = std::make_unique<BPlusOmap>(cap, cfg.bplus_order, bs);
        bp->set_index_mode(true);
        return bp;
    }
    case OmapBackend::DaAvl:
        return std::make_unique<DaOstOmap>(
            cap, OdsTreeType::AVL, 0, bs, 8);
    case OmapBackend::DaBplus:
        return std::make_unique<DaOstOmap>(
            cap, OdsTreeType::BPlus, 0, bs, cfg.bplus_order);
    default:
        if (is_cold && cfg.use_split_oram && hot_ref_count > 0) {
            int split_depth = ceil_log2(std::max(hot_ref_count, 2));
            return std::make_unique<AVLOmap>(
                cap, bs, split_depth, std::max(hot_ref_count, 1));
        }
        return std::make_unique<AVLOmap>(cap, bs);
    }
}

static Bytes export_omap(OmapInterface* omap, OmapBackend be) {
    switch (be) {
    case OmapBackend::AVL:
        return static_cast<AVLOmap*>(omap)->export_state();
    case OmapBackend::BPlus:
        return static_cast<BPlusOmap*>(omap)->export_state();
    case OmapBackend::DaAvl:
    case OmapBackend::DaBplus:
        return static_cast<DaOstOmap*>(omap)->export_state();
    }
    return {};
}

Bytes setup_standalone_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int bucket_size, int value_size,
    int init_n) {

    int data_count = init_n > 0 ? init_n : N;
    std::cerr << "[bench_setup] standalone: backend=" << (int)backend
              << " N=" << N << " init=" << data_count
              << " bs=" << bucket_size << " vs=" << value_size << "\n";

    auto data = make_data(data_count, value_size);
    auto omap = make_omap(backend, N, bucket_size);
    omap->init(data);

    // Export with placeholder store_ids (level/leaf_range are correct since storage still attached)
    auto omap_blob = export_omap(omap.get(), backend);

    // Detach storages, register, patch store_ids
    detach_register_patch_omap(stores, omap.get(), backend, omap_blob, 0);

    // Wrap: backend(4) + N(4) + bucket_size(4) + omap_blob_len(4) + omap_blob
    Bytes result;
    si(result, static_cast<int>(backend));
    si(result, N);
    si(result, bucket_size);
    si(result, static_cast<int>(omap_blob.size()));
    result.insert(result.end(), omap_blob.begin(), omap_blob.end());

    std::cerr << "[bench_setup] standalone done: stores=" << stores.stores.size()
              << " blob=" << result.size() << " bytes\n";
    return result;
}

Bytes setup_tiered_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int n, int bucket_size, int value_size,
    SecurityMode mode, bool use_split,
    OmapBackend hot_backend, bool use_hot_backend,
    bool epoch_encoded_values) {

    std::cerr << "[bench_setup] tiered: backend=" << (int)backend
              << " hot_be=" << (int)hot_backend << " use_hot=" << use_hot_backend
              << " index_meta=" << epoch_encoded_values
              << " N=" << N << " n=" << n << " vs=" << value_size << "\n";

    std::vector<int> hot_keys(n);
    for (int i = 0; i < n; ++i) hot_keys[i] = i;

    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = mode;
    cfg.use_split_oram = use_split;
    cfg.bucket_size = bucket_size;
    cfg.backend = backend;
    cfg.use_hot_backend = use_hot_backend;
    cfg.hot_backend = hot_backend;
    cfg.epoch_encoded_values = epoch_encoded_values;
    cfg.storage_creator = nullptr;
    cfg.data_storage_creator = make_data_storage_creator(stores);

    if (!epoch_encoded_values) {
        Bytes data_blob = setup_data_oram_blob(
            stores, N, bucket_size, value_size);

        std::vector<std::pair<int, Bytes>> hot_refs;
        std::vector<std::pair<int, Bytes>> cold_refs;
        hot_refs.reserve(static_cast<size_t>(n));
        cold_refs.reserve(static_cast<size_t>(std::max(N - n, 0)));
        for (int k = 0; k < N; ++k) {
            if (k < n)
                hot_refs.emplace_back(k, int_to_bytes(k));
            else
                cold_refs.emplace_back(k, int_to_bytes(k));
        }

        int hot_cap = std::max(static_cast<int>(hot_refs.size()), 1);
        int cold_cap = std::max(static_cast<int>(cold_refs.size()), 1);
        auto hot = make_tiered_index_omap(
            cfg.effective_hot_backend(), hot_cap, false, cfg,
            static_cast<int>(hot_refs.size()));
        auto cold = make_tiered_index_omap(
            backend, cold_cap, true, cfg,
            static_cast<int>(hot_refs.size()));
        hot->init(hot_refs);
        cold->init(cold_refs);

        Bytes hot_blob = export_omap(hot.get(), cfg.effective_hot_backend());
        detach_register_patch_omap(
            stores, hot.get(), cfg.effective_hot_backend(), hot_blob, 0);

        Bytes cold_blob = export_omap(cold.get(), backend);
        detach_register_patch_omap(stores, cold.get(), backend, cold_blob, 0);

        Bytes tm_blob;
        si(tm_blob, N);
        si(tm_blob, n);
        si(tm_blob, static_cast<int>(mode));
        si(tm_blob, use_split ? 1 : 0);
        si(tm_blob, bucket_size);
        si(tm_blob, static_cast<int>(backend));
        si(tm_blob, cfg.bplus_order);
        si(tm_blob, hot_cap);
        si(tm_blob, N); // next_data_block_id
        si(tm_blob, use_hot_backend ? 1 : 0);
        si(tm_blob, static_cast<int>(hot_backend));

        si(tm_blob, n);
        for (int k = 0; k < n; ++k) si(tm_blob, k);
        si(tm_blob, n);
        for (int k = 0; k < n; ++k) si(tm_blob, k);
        si(tm_blob, n);
        for (int k = 0; k < n; ++k) si(tm_blob, k);

        si(tm_blob, static_cast<int>(data_blob.size()));
        tm_blob.insert(tm_blob.end(), data_blob.begin(), data_blob.end());
        si(tm_blob, static_cast<int>(hot_blob.size()));
        tm_blob.insert(tm_blob.end(), hot_blob.begin(), hot_blob.end());
        si(tm_blob, static_cast<int>(cold_blob.size()));
        tm_blob.insert(tm_blob.end(), cold_blob.begin(), cold_blob.end());

        Bytes result;
        si(result, 1);
        si(result, static_cast<int>(tm_blob.size()));
        result.insert(result.end(), tm_blob.begin(), tm_blob.end());

        std::cerr << "[bench_setup] tiered done: stores="
                  << stores.stores.size()
                  << " blob=" << result.size() << " bytes\n";
        return result;
    }

    TieredOMap tm(cfg);
    tm.init_sequential_values(N, value_size, hot_keys);

    // Export full TieredOMap state
    auto tm_blob = tm.export_state();

    // Parse tm_blob to find data/hot/cold ORAM offsets, then detach/register/patch.
    // TieredOMap export layout:
    //   total_keys(4) hot_set_size(4) mode(4) use_split(4)
    //   bucket_size(4) backend(4) bplus_order(4) hot_capacity(4)
    //   next_data_block_id(4) use_hot_backend(4) hot_backend(4) = 44 bytes header
    //   hot_keys_size(4) + hot_keys[...]
    //   phys_hot_keys_size(4) + phys_hot_keys[...]
    //   hot_key_list_size(4) + hot_key_list[...]
    //   data_blob_len(4) + data_blob[...]   (shared data ORAM)
    //   hot_blob_len(4) + hot_blob[...]
    //   cold_blob_len(4) + cold_blob[...]

    const uint8_t* scan = tm_blob.data() + 44;
    int hk_sz; std::memcpy(&hk_sz, scan, 4); scan += 4 + hk_sz * 4;
    int phk_sz; std::memcpy(&phk_sz, scan, 4); scan += 4 + phk_sz * 4;
    int hkl_sz; std::memcpy(&hkl_sz, scan, 4); scan += 4 + hkl_sz * 4;

    int data_blob_len; std::memcpy(&data_blob_len, scan, 4); scan += 4;
    int data_sid_offset = static_cast<int>(scan - tm_blob.data());
    {
        auto s = tm.data_oram().detach_storage();
        int sid = register_storage(stores, std::move(s));
        patch_sid(tm_blob, data_sid_offset, sid);
    }
    scan += data_blob_len;

    int hot_blob_len; std::memcpy(&hot_blob_len, scan, 4); scan += 4;
    int hot_data_off = static_cast<int>(scan - tm_blob.data());

    OmapBackend hot_be = cfg.effective_hot_backend();
    detach_register_patch_omap(stores, tm.hot_omap(), hot_be, tm_blob, hot_data_off);
    scan += hot_blob_len;

    int cold_blob_len; std::memcpy(&cold_blob_len, scan, 4); scan += 4;
    int cold_data_off = static_cast<int>(scan - tm_blob.data());

    detach_register_patch_omap(stores, tm.cold_omap(), backend, tm_blob, cold_data_off);

    // Wrap: mode_tag(4=1 for tiered) + tm_blob
    Bytes result;
    si(result, 1); // tiered tag
    si(result, static_cast<int>(tm_blob.size()));
    result.insert(result.end(), tm_blob.begin(), tm_blob.end());

    std::cerr << "[bench_setup] tiered done: stores=" << stores.stores.size()
              << " blob=" << result.size() << " bytes\n";
    return result;
}

Bytes setup_index_data_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int bucket_size, int value_size) {

    std::cerr << "[bench_setup] index_data: backend=" << (int)backend
              << " N=" << N << " bs=" << bucket_size << " vs=" << value_size << "\n";

    // 1. Index OMAP with 4-byte reference values
    auto index = make_omap(backend, N, bucket_size);
    if (backend == OmapBackend::BPlus)
        static_cast<BPlusOmap*>(index.get())->set_index_mode(true);

    std::vector<std::pair<int, Bytes>> idata;
    idata.reserve(N);
    for (int i = 0; i < N; ++i)
        idata.emplace_back(i, int_to_bytes(i));
    index->init(idata);

    // 2. Data PathORAM with full-size values
    Bytes data_blob = setup_data_oram_blob(stores, N, bucket_size, value_size);

    // 3. Export blobs
    auto index_blob = export_omap(index.get(), backend);

    // 4. Detach/register/patch index OMAP stores
    detach_register_patch_omap(stores, index.get(), backend, index_blob, 0);

    // 6. Build result: backend(4) + N(4) + bucket_size(4)
    //                + index_blob_len(4) + index_blob
    //                + data_blob_len(4)  + data_blob
    Bytes result;
    si(result, static_cast<int>(backend));
    si(result, N);
    si(result, bucket_size);
    si(result, static_cast<int>(index_blob.size()));
    result.insert(result.end(), index_blob.begin(), index_blob.end());
    si(result, static_cast<int>(data_blob.size()));
    result.insert(result.end(), data_blob.begin(), data_blob.end());

    std::cerr << "[bench_setup] index_data done: stores=" << stores.stores.size()
              << " blob=" << result.size() << " bytes\n";
    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// Client-side restore
// ═══════════════════════════════════════════════════════════════════════

static std::unique_ptr<OmapInterface> restore_by_backend(
    OmapBackend be, const uint8_t*& p, std::shared_ptr<TcpChannel> ch) {
    switch (be) {
    case OmapBackend::AVL:    return AVLOmap::from_state(p, ch);
    case OmapBackend::BPlus:  return BPlusOmap::from_state(p, ch);
    case OmapBackend::DaAvl:
    case OmapBackend::DaBplus:
        return DaOstOmap::from_state(p, ch);
    }
    return nullptr;
}

std::unique_ptr<OmapInterface> restore_standalone(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    int backend_i = di(p);
    int N = di(p); (void)N;
    int bucket_size = di(p); (void)bucket_size;
    int blob_len = di(p);
    (void)blob_len;
    auto be = static_cast<OmapBackend>(backend_i);
    return restore_by_backend(be, p, channel);
}

std::unique_ptr<TieredOMap> restore_tiered(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    int tag = di(p); (void)tag; // should be 1
    int blob_len = di(p); (void)blob_len;
    return TieredOMap::from_state(p, channel);
}

IndexDataParts restore_index_data(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    int backend_i = di(p);
    int N = di(p); (void)N;
    int bucket_size = di(p); (void)bucket_size;

    int index_blob_len = di(p); (void)index_blob_len;
    auto be = static_cast<OmapBackend>(backend_i);
    auto index = restore_by_backend(be, p, channel);

    int data_blob_len = di(p); (void)data_blob_len;
    auto data = PathORAM::from_state_network(p, channel);

    return {std::move(index), std::move(data)};
}

}  // namespace bench_setup
}  // namespace tiered_omap
