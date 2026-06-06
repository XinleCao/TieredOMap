# TieredOMap 实验测试指南

> 文件布局、结果目录和当前主实验入口先看
> `docs/experiment_layout.md`。本文件保留更详细的历史实验命令和
> client/server 部署说明。当前 paper-facing 主线使用
> `scripts/run_revised_experiments.sh`；`run_paper_experiments.sh` 和
> `bench_paper` 中的 `bandwidth/modes/latency` 旧入口只作为
> legacy/diagnostic，不再作为 revised client/server 主实验。

## 一、环境准备

### 依赖
- C++17 编译器 (g++ 或 clang++)
- CMake >= 3.16
- OpenSSL (libssl-dev)
- Google Test (libgtest-dev)

### 编译 (在两台机器上都要做)

```bash
cd /path/to/TieredOMap
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) bench_paper oram_server
```

编译产物：
- `build/oram_server` — 存储服务端
- `build/bench_paper` — 所有实验的统一入口

---

## 二、部署架构

```
┌──────────────┐         TCP (port 12345)         ┌──────────────┐
│  Client 机器  │ ◄──────────────────────────────► │  Server 机器  │
│  bench_paper  │                                  │  oram_server  │
└──────────────┘                                   └──────────────┘
```

### Step 1：在 Server 机器上启动存储服务

```bash
# 默认端口 12345
bash scripts/run_server.sh

# 自定义端口
bash scripts/run_server.sh 9999
```

服务启动后会打印端口号和 PID，保持终端运行。

### Step 2：在 Client 机器上运行实验

```bash
# 当前 paper-facing 主线 (推荐先用小规模测试)
bash scripts/run_revised_experiments.sh 16 100 <SERVER_IP> 12345 results_revised_smoke

# 正式实验
bash scripts/run_revised_experiments.sh 20 500 <SERVER_IP> 12345 results_revised

# 历史 broad runner；只在需要重跑 legacy/diagnostic 数据时使用
bash scripts/run_paper_experiments.sh 20 500 <SERVER_IP> 12345
```

参数说明：
| 参数 | 含义 | 默认值 | 论文设置 |
|------|------|--------|---------|
| 第1个 | max_logN (最大 N=2^max_logN) | 20 | 20~24 |
| 第2个 | Q (每个配置的查询数) | 200 | 500~10000 |
| 第3个 | host (server IP) | 空=本地 | server IP |
| 第4个 | port | 12345 | 12345 |
| 第5个 | outdir (仅 revised runner) | 自动生成 | results_revised |

---

## 三、实验列表与论文映射

所有 CSV 输出在 `results_YYYYMMDD_HHMMSS/` 目录下。

### Revised Mainline: FO Client/Server + TEE Batch-TM

新的主实验入口避免把 per-query TM 作为 client/server 主模式：

```bash
bash scripts/run_revised_experiments.sh 20 500
```

如果 client/server 存储端在另一台机器上：

```bash
bash scripts/run_revised_experiments.sh 20 500 <SERVER_IP> 12345
```

输出：

| 输出 | 含义 |
|------|------|
| `client_server/client_fo.csv` | client/server 主线，只比较 standalone、fair baseline、TieredOMap-FO |
| `client_server_dynamic/client_dynamic_bw.csv` | client/server dynamic 扩展，只比较 static-FO 与 dynamic-FO 的 bandwidth 变化 |
| `tee_batch/tee_batch.csv` | TEE/T1 主线，比较 large/constrained trusted memory 下的 flat EnigMap-style AVL、TieredOMap-FO、TieredOMap-BatchTM |

Batch-TM 的代码口径是：每批 `beta` 个逻辑查询先全部经过 hot-index phase；命中的 hot results 作为一个 batch 释放；剩余 `beta-h` 个 cold continuations 再进入 cold OMAP，并作为 cold-result batch 释放。batch 内允许重复 key，不做预去重。

TEE/T1 的代码口径是 server-side only：`bench_tee_batch` 只计 TEE/server 内部处理时间，不要求实体 client，也不计网络 round-trip。CSV 中 `trusted_mem=large` 表示直接使用实测 server 时间；`trusted_mem=constrained` 表示在同一实测时间上按 tracked page touches 加入可配置页代价，默认 `trusted_kb=8192,page_us=8`。如果在真实 TEE/SGX 机器上跑，可以使用 `--env hardware`，此时 paper claim 应优先使用 `measured_*` 列。当前 runner 对 TEE/T1 使用 `--val 32`，把 OMAP value 解释为 data-reference/index metadata；`--val 256` 应作为 value-size sensitivity，而不是 index 主线。

为了避免把大量时间浪费在重复建树上，`bench_tee_batch` 默认 `--reuse_init 1`：每个 `logN` 和 mode 只初始化一次，然后复用同一个 initialized map 跑所有 beta。初始化不计入 query latency，但会通过 CSV 的 `init_us` 单独报告。如果需要严格恢复旧行为，可以传 `--reuse_init 0`，此时每个 beta/mode 都重新初始化。

关键列：

| 列 | 含义 |
|----|------|
| `trusted_mem` | `large` / `constrained` / `hardware` |
| `server_only` | 固定为 `1`，表示不包含实体 client/network 时间 |
| `mode` | `flat_enig` / `tiered_FO` / `tiered_BatchTM` |
| `init_policy,init_us` | 初始化复用策略和初始化耗时；不计入 `measured_*` |
| `measured_*_us` | 直接测到的 server-side 时间 |
| `modeled_*_us` | constrained-memory 模型后的时间；`large/hardware` 下等于 measured |
| `hot_working_kb,cold_working_kb,total_working_kb` | 内存模型使用的 estimated trusted working set |

Client/server dynamic bandwidth 的代码口径是：保持 FO 安全模式，分别运行 `static_fo` 和开启 fixed-rate maintenance 的 `dynamic_fo`。动态配置默认 `B_obs=256, B_swap=32, cache=8, piggyback=on`，并在正式测量前 warm up 至少一个 observation window，避免把尚未启动 maintenance 的阶段计入结果。该实验只解释动态维护带来的 bandwidth 增量；hot-set 恢复精度仍使用已有 dynamic convergence 数据。

下面的 Exp 1--10 是 `bench_paper` 的历史细项入口。`backend_cmp`、`dynamic`、`drift` 等仍可作为辅助数据；但 `bandwidth`、`latency`、`modes` 中的 per-query TM/TM+Split 不再作为 revised client/server 主线证据。新版 client/server 主线应优先使用 `client_server/client_fo.csv` 和 `client_server_dynamic/client_dynamic_bw.csv`。

### Exp 1: backend_cmp — 后端对比 (单体 OMAP)

```bash
./build/bench_paper --exp=backend_cmp --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `backend_cmp.csv` | Table 1 (tab:comparison) 的数据支撑；验证 AVL / B+ / DAORAM+B+ 三种 OMAP 的 bandwidth 和 rounds |

**关键数据列**：`logN, backend, avg_bw_KB, avg_rounds, avg_comp_us`

---

### Exp 2: bandwidth — legacy 带宽诊断

```bash
./build/bench_paper --exp=bandwidth --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `bandwidth_vs_N.csv` | legacy/diagnostic only；revised client/server scalability 使用 `client_server/client_fo.csv` |

**关键数据列**：`logN, backend, type(standalone/fair/TM_split), avg_bw_KB, avg_rounds`

该入口保留旧的 TM_split 诊断口径，不再用于证明 client/server revised 主线。

---

### Exp 3: skewness — Zipf 偏斜度的影响

```bash
./build/bench_paper --exp=skewness --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `skewness.csv` | **Figure 6** (fig:exp-skewness) — Q2: 不同 s 值下的加速比和 hit rate |

**关键数据列**：`zipf_s, backend, avg_answer_rnd, hit_pct`

固定 N=2^16, n=1024，s 从 0.5 扫到 1.5。

---

### Exp 4: hotsize — Hot-set 大小的影响

```bash
./build/bench_paper --exp=hotsize --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `hotsize.csv` | **Figure 7** (fig:exp-hotsize) — Q2: 不同 n 值下的带宽 |

**关键数据列**：`log_n, backend, avg_answer_rnd, avg_total_bw_KB`

固定 N=2^16, s=1.0，n 从 2^4 到 2^12。

---

### Exp 5: latency — legacy RTT 诊断

```bash
./build/bench_paper --exp=latency --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `latency.csv` | legacy/diagnostic only；revised client/server latency 从 `client_fo.csv` 的 answer/full rounds 或 TCP 实测导出 |

**关键数据列**：`rtt_ms, backend, type, avg_rounds, avg_answer_rnd, latency_ms, answer_latency_ms`

用解析方法计算：latency = rounds × RTT。在 TCP 模式下，实际网络延迟会自动体现。该入口可能混入旧模式配置，正式 revised 表述不要直接把它映射到 per-query TM 对比。

---

### Exp 6: modes — legacy per-query 模式诊断

```bash
./build/bench_paper --exp=modes --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `modes.csv` | legacy/diagnostic only；不作为 revised client/server 主线表格 |

**关键数据列**：`logN, backend, mode(FO/TM/TM+Split), avg_bw_KB, avg_rounds, avg_answer_rnd, hit_pct`

该实验中的 TM 是 per-query tier-membership：它会泄露单个查询是否 hot/cold。当前 revised 设计不再把它作为 client/server 主模式；tier membership 的 paper-facing 版本只放在 TEE BatchTM 中，以 batch-level hot/cold count 作为泄露口径。

---

### Exp 7: write — 读写开销对比

```bash
./build/bench_paper --exp=write --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `write_overhead.csv` | **Table 7** (tab:exp-write) — Q6: search / update / insert 的带宽和轮次 |

**关键数据列**：`backend, op(search/update/insert), avg_bw_KB, avg_rounds`

---

### Exp 8: dynamic — 动态 hot-set 维护收敛

```bash
./build/bench_paper --exp=dynamic --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `dynamic.csv` | **Figure 10** (fig:exp-converge) — Q5: 从随机 hot-set 开始，hit rate 随查询数收敛 |

**关键数据列**：`query_idx, backend, hit_pct, epoch`

配置：N=512, n=64, epoch=128, 总查询 3000。四种 backend 都会测试（AVL/B+/DAORAM+AVL/DAORAM+B+），DAORAM 系列使用 piggyback scan。

---

### Exp 9: workload — YCSB 分布对比

```bash
./build/bench_paper --exp=workload --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `workload_dist.csv` | 新增实验（审稿意见要求）— 对比 Zipfian / Uniform / Latest 三种 YCSB 分布下的性能 |

**关键数据列**：`distribution, backend, avg_bw_kb, avg_answer_rnd, hit_pct`

Uniform 分布下 hit rate 接近 n/N（无偏斜优势），Latest 分布下 hit rate 取决于最近插入的 key 是否在 hot set 中。

---

### Exp 10: drift — 负载漂移适应性

```bash
./build/bench_paper --exp=drift --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `drift.csv` | **Figure 11** (fig:exp-drift) — Q5: 在 query 2000 处切换热点分布，测试恢复速度 |

**关键数据列**：`query_idx, backend, scenario(no_maint/with_maint), hit_pct`

对比有无动态维护两种场景。

---

## 四、推荐执行顺序

### 快速验证（约 10 分钟）

先跑 revised 小规模确认 server/client 通信正常：

```bash
# Server
bash scripts/run_server.sh

# Client (另一个终端或机器)
bash scripts/run_revised_experiments.sh 14 50 <SERVER_IP> 12345 results_revised_smoke
```

### 正式实验（约 2-4 小时）

```bash
bash scripts/run_revised_experiments.sh 20 500 <SERVER_IP> 12345 results_revised
```

如需更大规模 (N 到 2^24)，注意 DAORAM backend 在 N>2^20 时初始化较慢：

```bash
bash scripts/run_revised_experiments.sh 24 1000 <SERVER_IP> 12345 results_revised_large
```

### 单独重跑某个实验

```bash
# 只跑 dynamic 实验
./build/bench_paper --exp=dynamic --outdir=results_rerun --host=<IP>

# 只跑 legacy bandwidth 诊断，自定义参数
./build/bench_paper --exp=bandwidth --Q=1000 --max_logN=22 --outdir=results_bw --host=<IP>
```

---

## 五、输出文件总结

### Revised paper-facing 输出

| CSV 文件 | 用途 |
|----------|------|
| `client_server/client_fo.csv` | client/server static 主线：standalone、fair baseline、TieredOMap-FO |
| `client_server_dynamic/client_dynamic_bw.csv` | client/server dynamic 扩展：static-FO vs dynamic-FO bandwidth |
| `tee_batch/tee_batch.csv` | TEE/T1 主线：large/constrained trusted memory 下的 FO 与 BatchTM |

### Historical / diagnostic 输出

| CSV 文件 | 论文图表 | 对应问题 |
|----------|---------|---------|
| `backend_cmp.csv` | Table 1 | 后端基准 |
| `bandwidth_vs_N.csv` | legacy only | 旧 TM_split 带宽诊断 |
| `skewness.csv` | Figure 6 | Q2: 偏斜敏感性 |
| `hotsize.csv` | Figure 7 | Q2: hot-set 大小 |
| `latency.csv` | legacy only | 旧 RTT 诊断 |
| `modes.csv` | legacy only | 旧 per-query TM/TM+Split 诊断 |
| `write_overhead.csv` | Table 7 | Q6: 读写一致性 |
| `dynamic.csv` | Figure 10 | Q5: 动态维护收敛 |
| `workload_dist.csv` | 新增 | YCSB 分布对比 |
| `drift.csv` | Figure 11 | Q5: 负载漂移 |

---

## 六、资源估算（内存 & 时间）

下表是历史 broad runner 的估算，用于判断 legacy/diagnostic 细项是否会跑爆机器。revised runner 的 paper-facing 主线只需要关注 `client_fo`、`client_dynamic_bw` 和 `tee_batch` 三组输出。实际时间受网络 RTT 影响较大：LAN 下以计算为主，WAN-80 下以网络等待为主。

**内存说明**：每个 Path ORAM tree (capacity N, bucket_size Z=4) 的 server 端内存约 `2N × Z × block_size ≈ 1.5~2 KB × N`。递归 position map 再加约 10-15%。DAORAM backend 因为额外的去摊销结构，内存约为 AVL/B+ 的 1.5 倍。TieredOMap 同时持有 3 个 ORAM instance（hot、cold-up、cold-low），但 hot 和 cold-up 容量仅为 n，远小于 N。各实验按顺序运行，峰值内存取单次最大配置。

### max_logN = 20, Q = 500（推荐首次运行）

| # | 实验 | 峰值 N | ORAM 实例数 | Server 峰值内存 | Client 内存 | 预估时间 |
|---|------|--------|------------|----------------|------------|---------|
| 1 | backend_cmp | 2^20 | 1 | ~2 GB | ~200 MB | 15-25 min |
| 2 | bandwidth (legacy) | 2^20 | 3 (tiered) | ~3 GB | ~300 MB | 25-40 min |
| 3 | skewness | 2^16 | 3 | ~300 MB | ~100 MB | 8-12 min |
| 4 | hotsize | 2^16 | 3 | ~300 MB | ~100 MB | 8-12 min |
| 5 | latency (legacy) | 2^16 | 3 | ~300 MB | ~100 MB | 8-12 min |
| 6 | modes (legacy) | 2^20 | 3 | ~3 GB | ~300 MB | 30-50 min |
| 7 | write | 2^16 | 1 | ~200 MB | ~80 MB | 5-8 min |
| 8 | dynamic | 512 | 3 | ~30 MB | ~10 MB | 3-5 min |
| 9 | workload | 2^16 | 3 | ~300 MB | ~100 MB | 8-12 min |
| 10 | drift | 512 | 3 | ~30 MB | ~10 MB | 3-5 min |
| | **合计** | | | **峰值 ~3 GB** | **峰值 ~300 MB** | **~2-3 小时** |

### max_logN = 24, Q = 1000（大规模实验）

| # | 实验 | 峰值 N | Server 峰值内存 | 预估时间 |
|---|------|--------|----------------|---------|
| 1 | backend_cmp | 2^24 | ~30 GB | 2-4 hr |
| 2 | bandwidth (legacy) | 2^24 | ~35 GB | 3-5 hr |
| 6 | modes (legacy) | 2^24 | ~35 GB | 4-6 hr |
| 3-5,7-10 | 其余 | ≤ 2^16 | < 1 GB | ~1 hr |
| | **合计** | | **峰值 ~35 GB** | **~10-16 小时** |

### 关键瓶颈说明

- **初始化时间**：OMAP init 需要逐一插入 N 条数据，N=2^20 时约 1-3 分钟，N=2^24 时约 30-60 分钟。DAORAM backend 因去摊销结构额外慢 30-50%。
- **backend_cmp / legacy bandwidth / legacy modes** 是历史 broad runner 中三个最耗时的实验，因为它们遍历多个 logN 值，每个值都要重新 init。
- **dynamic / drift** 用小 N (512) 跑 3000-5000 次查询，几分钟即可完成。
- **TCP 网络开销**：LAN 环境下 TCP 开销可忽略（<5%）。WAN-80 环境下每次 ORAM 访问多 ~80ms RTT，实验时间会显著增加（约 3-5 倍）。

### 资源规划建议

| 场景 | Server 机器 | Client 机器 | 总时间 |
|------|------------|------------|--------|
| 快速验证 (logN≤14, Q=50) | 1 GB RAM | 512 MB RAM | ~10 min |
| 标准实验 (logN≤20, Q=500) | **8 GB RAM** | 2 GB RAM | ~2-3 hr |
| 大规模 (logN≤24, Q=1000) | **64 GB RAM** | 4 GB RAM | ~10-16 hr |

> 建议周一先跑 `logN≤20, Q=500` 的标准实验，确认数据合理后再跑 `logN=24` 的大规模实验。大规模实验可以晚上挂着跑。

---

## 七、注意事项

1. **Server 不需要重启**：`oram_server` 支持多次 client 连接，每次实验 client 断开后 server 会自动清理状态。
2. **网络延迟**：如果两台机器在同一局域网，RTT 约 0.1ms (LAN)。如果要模拟 WAN，可用 `tc` 命令加延迟：
   ```bash
   # 在 client 机器上模拟 80ms RTT
   sudo tc qdisc add dev eth0 root netem delay 40ms
   # 清除
   sudo tc qdisc del dev eth0 root netem
   ```
3. **TEE 实验**：需要 SGX 硬件，不在 `bench_paper` 中。需要单独的 SGX enclave 编译流程（待后续补充）。
4. **论文中的实验数据**：当前论文中的 Figure/Table 包含手工填写的示意数据。跑完实验后用实际 CSV 数据替换。
