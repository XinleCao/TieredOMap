# TEE EnigMap Experiment Summary

Run: `tee_original_enigmap_n1024_s10_20260608_231901`

Configuration:

- Server: Alibaba Cloud SGX instance, 64 vCPUs, 128 GB normal memory, 128 GB trusted memory.
- Hot tier: `n = 1024`.
- Workload: Zipf `s = 1.0`, `Q = 200` queries per point.
- Scales: `N in {2^16, 2^20, 2^24}`.
- Profiles:
  - `256B_map`: map-side workload following EnigMap, no data ORAM.
  - `4KB_full_query`: index/data-separated workload with a disk-backed 4 KB data ORAM.

Headline results:

- `256B_map`: FO answer time reduces EnigMap server-side latency by 37.7--52.1%; BatchTM reduces total server-side latency by 41.5--57.1%.
- `4KB_full_query`: FO answer time reduces EnigMap server-side latency by 39.6--61.2%; BatchTM reduces total server-side latency by 39.5--62.8%.

The reusable large data-ORAM setup was kept on the server and was not copied into this repository. Only CSVs and logs are archived here.
