# TieredOMap

Exploiting access skewness for efficient oblivious key-value stores.

## Overview

TieredOMap partitions keys into independent **hot** and **cold** oblivious maps (OMAPs) and searches both in parallel on every query. It offers two operating modes:

- **Full Obliviousness** — identical security to a standard OMAP (zero additional leakage), yet the client receives hot-key answers after only O(log n) rounds instead of O(log N).
- **Tier-Membership Privacy** — reveals one bit (hot or cold) per query, reducing hot-key bandwidth to O(log²n + log N) via Split-ORAM.

The system is agnostic to the underlying OMAP construction: any OMAP (AVL-based, B⁺-tree-based, etc.) can be used as a drop-in component.

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Requires C++17 and [Google Test](https://github.com/google/googletest) (installed via Homebrew or system package manager).

## Running Tests

```bash
cd build
ctest --output-on-failure
```

## Running Benchmarks

```bash
cd build

# 基础带宽/延迟实验（本地已测到 N=2^22）
./bench_latency --exp=bandwidth --Q=200 --warmup=50

# 大规模实验（逐个跑，节省内存）
./bench_large 22   # N=2^22, ~3.5GB RAM, ~50s

# Round reduction 实验
./bench_rounds --exp=rounds --Q=200

# 动态维护实验
./bench_dynamic --exp=hitrate --Q=500 --epoch=256
```

## Project Structure

```
include/tiered_omap/
  common.h              # Types, bandwidth stats, EpochMeta, utilities
  maintenance.h         # Dynamic hot-set maintenance (epoch-based)
  workload.h            # ZipfSampler, ShiftingZipfSampler
  oram/
    path_oram.h         # Path ORAM with bandwidth tracking + RTT injection
    da_oram.h           # De-amortized ORAM (DAORAM)
    binary_tree_storage.h
  omap/
    omap_interface.h    # OMAP interface (incl. partial_dummy_access)
    avl_omap.h          # AVL OMAP with Split-ORAM support
    bplus_omap.h        # B+ tree OMAP
  tiered_omap.h         # TieredOMap: parallel traversal + two security modes

src/                    # Implementations
tests/                  # Google Test suites
benchmark/
  bench_tiered_omap.cpp # 基础 bandwidth 实验
  bench_latency.cpp     # 综合实验：bandwidth / skewness / hotsize / latency / modes / ablation / write
  bench_large.cpp       # 大规模单点测量（一次只创建一个配置，节省内存）
  bench_rounds.cpp      # Round reduction / latency vs skewness / answer distribution
  bench_dynamic.cpp     # 动态维护：hit rate / drift / sensitivity
  bench_comptime.cpp    # 纯计算时间测量（无 sleep）
scripts/
  run_experiments.sh    # 自动化实验脚本
  plot_figures.py       # CSV → pgfplots LaTeX 数据
```

---

## 当前实验数据状态

### 已有实测数据（本地 M3 16GB，N ≤ 2^22）

| 实验 | 范围 | 状态 | 数据质量 |
|------|------|------|----------|
| Bandwidth vs N | N=2^12 ~ 2^22 | ✅ 已测 | 真实数据 |
| Skewness effect | s=0.5 ~ 1.5, N=2^16 | ✅ 已测 | 真实数据 |
| Hot-set size | n=2^4 ~ 2^12, N=2^16 | ✅ 已测 | 真实数据 |
| Split-ORAM ablation | N=2^14 ~ 2^22 | ✅ 已测 | 真实数据 |
| Write overhead | N=2^16 | ✅ 已测 | 真实数据 |
| Mode comparison (WAN) | N=2^16 | ✅ 已测 | rounds×RTT 解析推导 |

### 缺失数据（需要服务器）

| 实验 | 优先级 | 需要什么 |
|------|--------|----------|
| 大规模 N=2^24 ~ 2^26 | **P0** | ≥64GB RAM 服务器 |
| 真实 client-server 网络延迟 | **P0** | 两台机器 + tc/netem |
| SGX/TEE 实验 | **P0** | SGX 机器 |
| 动态 hot-set 维护实验 | **P1** | 可在服务器上跑 |
| Real-world trace (YCSB / Facebook) | **P1** | 下载 trace + 大内存 |
| Pancake / Waffle 对比 | **P2** | 复现/获取他们的代码 |

---

## 已知代码问题（需在服务器上修复）

### P0 — 必须修复

1. ~~**无加解密**~~ ✅ **已完成**
   - PathORAM 已集成 AES-128-CTR 加密（OpenSSL 3.x）
   - 构造时自动生成随机 AES key；写入 storage 前加密 block value，读出后解密
   - Stash 始终持有明文；对 rounds/bandwidth 无影响，增加微秒级计算开销
   - 所有 5 个 test suite 通过验证

2. **无 client-server 网络**
   - 论文写 "over TCP sockets"，但一切在本地单进程运行
   - 延迟实验用 rounds×RTT 解析推导，或用 `sleep_for()` 模拟
   - **修复**：实现简单 TCP 通信层，server 持有 ORAM 树，client 发 read/write path 请求

3. **无 SGX 实现**
   - 论文写 "Intel SGX SDK"，但没有 enclave 代码
   - TEE 数据全部是估算值
   - **修复**：实现 SGX enclave wrapper，测真实 EPC page faults 和 execution time

4. **动态 hot-set 是虚拟维护**
   - `do_maintenance_step()` 只更新逻辑 `hot_keys_` 集合，不物理搬运数据
   - `phys_hot_keys_` 始终不变——数据物理上没移动过
   - 原因：AVL OMAP 的 `remove()` 后 `insert()` 到另一棵树会导致 Path ORAM stash 不一致
   - **修复方案**：
     - 选项 A：修复 AVL OMAP remove 的 stash 管理（根本修复）
     - 选项 B：用 "lazy delete" + 标记位实现物理迁移（避免真正删除 AVL 节点）
     - 选项 C：对 B+ tree OMAP 实现物理搬运（叶子扫描天然支持）
   - 当前虚拟维护对 **hit rate 统计是准确的**，但不反映真实搬运开销

### P1 — 重要改进

5. **数据规模受限**
   - 本地 M3 16GB 最多跑到 N=2^22 (4M entries, ~3.5GB)
   - VLDB 通常要求 N=2^24 (16M) 以上
   - N=2^22 到 2^24 的趋势可以解析外推（rounds 是确定性的），但需要实测验证

6. ~~**B+ tree OMAP 未集成到 TieredOMap**~~ ✅ **已完成**
   - `TieredOMapConfig` 新增 `OmapBackend::BPlus` 选项和 `bplus_order` 参数
   - `TieredOMap::init()` 根据 backend 自动创建 AVL 或 B+ tree OMAP
   - 新增 3 个 B+ tree backend 测试全部通过

7. **Workload 多样性不足**
   - 只有 Zipf 合成工作负载
   - 需要 YCSB、Facebook Memcached trace、time-varying skew

8. **缺少 baseline 对比**
   - 没有和 Pancake / Waffle 的性能对比（即使安全模型不同，性能对比有参考价值）
   - 没有和 EnigMap / BOLT 的 TEE-OMAP 对比

### P2 — 加分项

9. **多线程/并发 client**
   - 论文未讨论多 client 场景
   - VLDB reviewer 可能问 server-side scalability

10. **Crash recovery / migration atomicity**
    - 动态维护中 promote/demote 如果中途 crash，key 可能 duplicate 或丢失
    - 需要讨论或实现原子性保证

---

## 论文写作待修改（对应 Stanford Reviewer 反馈）

- [ ] Split-ORAM 措辞修正：Section 4.2.1 "determine absence" → 强调 fixed-shape dummy access
- [ ] Tier-membership mode timing/packet leakage：讨论 abort cold traversal 的 TCP side channel + mitigation
- [ ] Migration 原子性和并发：说明 parallel tier probes 与 in-flight migration 的 race handling
- [ ] SelectTier policy：说明实验中具体策略，讨论 non-stationary skew 敏感性
- [ ] TEE threat model limitation：acknowledge cache/timing side-channel 作为 limitation
- [ ] Server-side 开销：补充 CPU、多线程、多 client 并发讨论

---

## 服务器上的工作计划

### 第一步：基础设施（1-2 天）
1. ~~加入 AES-128 加密（OpenSSL）~~ ✅ 已完成
2. 实现 TCP client-server 通信
3. 验证加密后 bandwidth/rounds 不变，测量加密开销

### 第二步：大规模实验（2-3 天）
1. 跑 N=2^24, 2^26 的 bandwidth/rounds 实验
2. 真实网络延迟测量（LAN / WAN via tc netem）
3. 用真实数据替换论文中所有解析推导值

### 第三步：SGX 实验（2-3 天）
1. SGX enclave wrapper
2. Packed-array hot index 实现
3. 测 EPC page faults 和 execution time

### 第四步：动态维护（3-5 天）
1. 修复物理搬运（优先 B+ tree backend）
2. 跑 hit rate convergence、workload drift、sensitivity 实验
3. 测搬运开销对 throughput 的影响

### 第五步：补充实验（2-3 天）
1. YCSB / real trace
2. ~~B+ tree backend 验证 backend-agnostic~~ ✅ 已集成 + 测试通过
3. 有条件的话对比 Pancake/Waffle

---

## Citation

Paper in preparation for VLDB 2027.

## License

This project is for academic research purposes.
