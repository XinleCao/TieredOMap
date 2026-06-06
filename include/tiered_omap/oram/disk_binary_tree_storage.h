#pragma once

#include "tiered_omap/oram/binary_tree_storage.h"
#include <mutex>
#include <string>
#include <unordered_set>

namespace tiered_omap {

class DiskBinaryTreeStorage : public StorageInterface {
public:
    DiskBinaryTreeStorage(int num_data, int bucket_size,
                          std::string directory);
    ~DiskBinaryTreeStorage() override;

    DiskBinaryTreeStorage(const DiskBinaryTreeStorage&) = delete;
    DiskBinaryTreeStorage& operator=(const DiskBinaryTreeStorage&) = delete;

    int level() const override { return level_; }
    int leaf_range() const override { return leaf_range_; }
    int bucket_size() const override { return bucket_size_; }

    void reset(int num_data, int bucket_size) override;
    void fill_data_to_leaf(const Block& block) override;

    std::unordered_set<int>
    bulk_load(const std::vector<Block>& blocks) override;

    void bulk_load_generated(
        int count, size_t max_value_size,
        const std::function<Block(int)>& make_block,
        const std::function<void(int)>& on_overflow) override;

    std::unordered_map<int, std::vector<Block>>
    read_path(int leaf) const override;

    void write_path(
        int leaf,
        const std::unordered_map<int, std::vector<Block>>& buckets) override;

    std::unordered_map<int, std::vector<Block>>
    read_multiple_paths(const std::vector<int>& leaves) const override;

    void write_multiple_paths(
        const std::unordered_map<int, std::vector<Block>>& buckets) override;

    void write_state(std::ostream& out) const;
    static std::unique_ptr<DiskBinaryTreeStorage>
    read_state(std::istream& in, const std::string& directory);

    void save_file_snapshot(const std::string& meta_path,
                            const std::string& data_path) const;
    static std::unique_ptr<DiskBinaryTreeStorage>
    load_file_snapshot(const std::string& meta_path,
                       const std::string& data_path,
                       const std::string& runtime_directory);

    static std::unique_ptr<DiskBinaryTreeStorage>
    from_binary_tree(const BinaryTreeStorage& src,
                     const std::string& directory);

private:
    DiskBinaryTreeStorage() = default;

    int leaf_to_node(int leaf) const { return leaf_range_ - 1 + leaf; }
    static int parent(int node) { return (node - 1) / 2; }

    static std::string make_file_path(const std::string& directory);
    uint64_t slot_size() const;
    uint64_t node_size() const;
    uint64_t node_offset(int node) const;
    uint64_t slot_offset(int node, int slot) const;

    std::vector<Block> read_bucket(int node) const;
    std::vector<Block> read_bucket_from_file(const std::string& path,
                                             int node) const;
    void write_bucket(int node, const std::vector<Block>& bucket);
    void write_slot(int node, int slot, const Block& block, int new_count);
    void write_slot_to(std::ostream& out, int node, int slot,
                       const Block& block, int new_count) const;
    void write_block_record(std::ostream& out, const Block& block) const;
    Block read_block_record(std::istream& in) const;
    void create_empty_file();

    int level_ = 0;
    int leaf_range_ = 0;
    int bucket_size_ = 0;
    int total_nodes_ = 0;
    size_t slot_value_size_ = 0;
    std::string directory_;
    std::string file_path_;
    std::string base_file_path_;
    mutable std::unordered_set<int> dirty_nodes_;
    bool remove_on_destroy_ = true;
    mutable std::mutex mutex_;
};

}  // namespace tiered_omap
