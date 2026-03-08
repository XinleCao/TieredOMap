# TieredOMap 实验测试指南

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
# 一键跑全部 10 个实验 (推荐先用小规模测试)
bash scripts/run_paper_experiments.sh 16 100 <SERVER_IP>

# 正式实验 (大规模)
bash scripts/run_paper_experiments.sh 20 500 <SERVER_IP> 12345
```

参数说明：
| 参数 | 含义 | 默认值 | 论文设置 |
|------|------|--------|---------|
| 第1个 | max_logN (最大 N=2^max_logN) | 20 | 20~24 |
| 第2个 | Q (每个配置的查询数) | 200 | 500~10000 |
| 第3个 | host (server IP) | 空=本地 | server IP |
| 第4个 | port | 12345 | 12345 |

---

## 三、实验列表与论文映射

所有 CSV 输出在 `results_YYYYMMDD_HHMMSS/` 目录下。

### Exp 1: backend_cmp — 后端对比 (单体 OMAP)

```bash
./build/bench_paper --exp=backend_cmp --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `backend_cmp.csv` | Table 1 (tab:comparison) 的数据支撑；验证 AVL / B+ / DAORAM+AVL / DAORAM+B+ 四种 OMAP 的 bandwidth 和 rounds |

**关键数据列**：`logN, backend, avg_bw_KB, avg_rounds, avg_comp_us`

---

### Exp 2: bandwidth — 带宽 & 交互轮次 vs N

```bash
./build/bench_paper --exp=bandwidth --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `bandwidth_vs_N.csv` | **Figure 5** (fig:exp-bandwidth) — Q1: 带宽随 N 增长的趋势 |

**关键数据列**：`logN, backend, type(standalone/tiered), avg_bw_KB, avg_rounds`

对每个 backend，比较 standalone OMAP 和 TieredOMap 的带宽差异。

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

### Exp 5: latency — 端到端延迟 (rounds × RTT)

```bash
./build/bench_paper --exp=latency --Q=500 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `latency.csv` | **Figure 8** (fig:exp-latency) + **Table 4** (tab:exp-modes) — Q3: 不同 RTT 下的实际延迟 |

**关键数据列**：`rtt_ms, backend, type, avg_rounds, avg_answer_rnd, latency_ms, answer_latency_ms`

用解析方法计算：latency = rounds × RTT。在 TCP 模式下，实际网络延迟会自动体现。

---

### Exp 6: modes — 安全模式对比 (FO vs TM vs TM+Split)

```bash
./build/bench_paper --exp=modes --Q=500 --max_logN=20 --host=<IP>
```

| 输出 | 论文位置 |
|------|---------|
| `modes.csv` | **Table 4** (tab:exp-modes) + **Table 6** (tab:exp-split) — Q3 & Q6: 模式对比和 Split-ORAM 消融 |

**关键数据列**：`logN, backend, mode(FO/TM/TM+Split), avg_bw_KB, avg_rounds, avg_answer_rnd, hit_pct`

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

先跑小规模确认 server/client 通信正常：

```bash
# Server
bash scripts/run_server.sh

# Client (另一个终端或机器)
bash scripts/run_paper_experiments.sh 14 50 <SERVER_IP>
```

### 正式实验（约 2-4 小时）

```bash
bash scripts/run_paper_experiments.sh 20 500 <SERVER_IP>
```

如需更大规模 (N 到 2^24)，注意 DAORAM backend 在 N>2^20 时初始化较慢：

```bash
bash scripts/run_paper_experiments.sh 24 1000 <SERVER_IP>
```

### 单独重跑某个实验

```bash
# 只跑 dynamic 实验
./build/bench_paper --exp=dynamic --outdir=results_rerun --host=<IP>

# 只跑 bandwidth 实验，自定义参数
./build/bench_paper --exp=bandwidth --Q=1000 --max_logN=22 --outdir=results_bw --host=<IP>
```

---

## 五、输出文件总结

| CSV 文件 | 论文图表 | 对应问题 |
|----------|---------|---------|
| `backend_cmp.csv` | Table 1 | 后端基准 |
| `bandwidth_vs_N.csv` | Figure 5 | Q1: 可扩展性 |
| `skewness.csv` | Figure 6 | Q2: 偏斜敏感性 |
| `hotsize.csv` | Figure 7 | Q2: hot-set 大小 |
| `latency.csv` | Figure 8, Table 4 | Q3: 延迟 |
| `modes.csv` | Table 4, Table 6 | Q3 & Q6: 模式 & Split |
| `write_overhead.csv` | Table 7 | Q6: 读写一致性 |
| `dynamic.csv` | Figure 10 | Q5: 动态维护收敛 |
| `workload_dist.csv` | 新增 | YCSB 分布对比 |
| `drift.csv` | Figure 11 | Q5: 负载漂移 |

---

## 六、注意事项

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
