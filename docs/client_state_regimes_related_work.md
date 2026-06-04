# Client-State Regimes and Related Work

This note organizes literature support for SOMAP's client-state regimes,
trusted-memory settings, and execution modes. The main conclusion is that
client-side or trusted-side storage is already a standard design axis in ORAM,
oblivious database, and encrypted-database systems. SOMAP should be presented
as a system that makes this axis explicit for skewed encrypted key-value
workloads, not as a design that assumes unlimited client storage.

## Executive Takeaways

1. Minimal client state has strong precedent in ORAM. Path ORAM explicitly
   targets small client storage, and snapshot-oblivious RAM studies security
   where recent-operation state is bounded by the snapshot window.

2. A local index without local values also has precedent. Epsolute keeps an
   index on the trusted user side to derive true record identifiers, while the
   actual records are fetched through ORAM. This is close to SOMAP's "hot index
   local, values remote" regime.

3. TEE-resident ORAM/OMAP state has strong precedent. Oblix places the ORAM
   client inside an enclave and then addresses leakage from the ORAM client's
   internal state. ObliDB similarly treats the enclave as a small trusted client
   and studies limited oblivious memory budgets.

4. The cleanest paper framing is a three-regime storage ladder:
   minimal client state, minimal state plus hot index, and TEE-resident state.
   The last regime should be evaluated under both large trusted memory and
   constrained trusted memory.

## SOMAP Client-State Regimes

| SOMAP regime | Client/trusted state | Closest precedents | How to use in the paper |
| --- | --- | --- | --- |
| Minimal state | Keys, stash, counters, and small protocol statistics; no key-level hot index. | Path ORAM; snapshot-oblivious RAM. | Use this as the conservative outsourced-storage setting. It supports the claim that small client state is a normal ORAM design goal. |
| Minimal state plus hot index | The client stores a sparse/hot index or hot membership metadata, but not hot values. Values still go through the data ORAM. | Epsolute's local index and DP structures; snapshot-oblivious RAM's recent-operation state. | This is the strongest match for SOMAP's practical client-side acceleration story. It lets SOMAP exploit skew without assuming that large values fit locally. |
| TEE-resident state | The ORAM/OMAP client, position-map fragments, stashes, and possibly hot/cold metadata reside inside a TEE. | Oblix; ObliDB; enclave-based ORAM systems. | Use this as the server-side trusted-execution setting. It must be split into large-memory and constrained-memory variants. |

Important wording: for the second regime, avoid saying that "hot values are
cached locally" unless the protocol actually stores them. The safer and more
defensible statement is that the client stores a hot index or hot membership
structure, while value retrieval remains protected by the data ORAM.

## Trusted-Memory Settings

### Large trusted memory

This setting models a modern server-grade TEE or a deployment where the trusted
working set fits without expensive paging. It is appropriate for experiments
that ask what SOMAP can do when the trusted state budget is not the bottleneck.

Relevant precedent:

- Recent DBMS-oriented TEE discussions distinguish early small-EPC SGX from
  newer server-grade SGX support. They explicitly identify secure-memory size
  and paging as major performance factors for database workloads.
- ObliDB evaluates algorithms under bounded oblivious memory and shows that
  additional oblivious memory can improve performance for some operators.

### Constrained trusted memory

This setting models early SGX-style EPC limits, mobile/edge clients, or a cloud
tenant that can only afford a small trusted working set. It is important because
SOMAP's advantage should not depend only on "everything fits in the enclave."

Relevant precedent:

- Path ORAM was designed around small client storage.
- ObliDB explicitly states that its oblivious memory can be as small as a few
  megabytes, at a performance cost.
- Snapshot-oblivious RAM gives a formal example where client storage is
  parameterized by the snapshot window rather than the full database size.

## Execution Modes

| Mode | Leakage | Literature support | SOMAP interpretation |
| --- | --- | --- | --- |
| FO, full-oblivious | Per-request tier membership is hidden. Each request follows a fixed observable template. | Path ORAM, Oblix, ObliDB's oblivious/padding modes. | Highest-security mode; best for private contact discovery, key transparency, medical/financial lookups, and other sensitive point-query workloads. |
| TM, tier-membership | The server may learn per-request tier membership or equivalent early termination information. | Searchable encryption and adjustable-leakage systems such as SEAL. | Performance-oriented mode with a clear leakage function. Use this when per-query hot/cold leakage is acceptable. |
| Batch-level tier membership | The server sees only an aggregate batch statistic, such as the number of hot hits in a public batch of size beta, but not which individual requests hit. | Epoch/batch processing in Obladi; ORAM request batching and volume/noise accounting in Epsolute; volume-leakage discussions in encrypted databases. | Intermediate mode between FO and TM. It leaks batch-level skewness, not individual hot/cold membership. |

The batch mode should be described as analogous to prior epoch/batch and
adjustable-leakage systems, not as identical to them. SOMAP's specific leakage is
the batch-level hit count or skewness, while individual request membership is
hidden within the batch.

## Paper-Ready Claim

SOMAP can be framed as follows:

> Prior ORAM and encrypted-database systems already treat trusted client state
> as a configurable resource: Path ORAM minimizes client storage, Epsolute keeps
> trusted-side indexes while storing records remotely, and enclave-based systems
> such as Oblix and ObliDB place ORAM or query-processing state inside a TEE.
> SOMAP follows this line but separates the storage of hot metadata from the
> storage of hot values. This distinction is important for key-value stores with
> large values: the client may keep only a hot index, while value accesses remain
> protected by a data ORAM. We therefore evaluate SOMAP under three client-state
> regimes and, for TEE deployment, under both large and constrained trusted-memory
> budgets.

## Citation Notes

Use these citations for the paper's related-work and evaluation-framing text.

- Path ORAM: Stefanov et al., "Path ORAM: An Extremely Simple Oblivious RAM
  Protocol," CCS 2013. The abstract explicitly emphasizes small client storage.
  <https://arxiv.org/abs/1202.5150>

- Snapshot-Oblivious RAM: Du, Genkin, and Grubbs, "Snapshot-Oblivious RAMs:
  Sub-Logarithmic Efficiency for Short Transcripts," CRYPTO 2022. This is useful
  for bounded-window security and client state parameterized by the snapshot
  length.
  <https://eprint.iacr.org/2022/858.pdf>

- Oblix: Mishra et al., "Oblix: An Efficient Oblivious Search Index," IEEE S&P
  2018. This supports enclave-resident ORAM clients, doubly-oblivious internal
  state, oblivious caching, and applications such as private contact discovery
  and key transparency.
  <https://people.eecs.berkeley.edu/~raluca/oblix.pdf>

- ObliDB: Eskandarian et al., "ObliDB: Oblivious Query Processing for Secure
  Databases," VLDB 2019/2020. This supports limited oblivious memory inside an
  enclave, flat/indexed/both storage choices, and performance dependence on
  trusted-memory budgets.
  <https://people.eecs.berkeley.edu/~matei/papers/2020/vldb_oblidb.pdf>

- Epsolute: Bogatov et al., "Epsolute: Efficiently Querying Databases While
  Providing Differential Privacy," CCS 2021. This is the closest precedent for a
  trusted-side local index plus remote ORAM-protected records, and it also
  discusses storage usage and ORAM request batching.
  <https://dbogatov.org/assets/docs/epsolute.pdf>

- SEAL: Demertzis et al., "SEAL: Attack Mitigation for Encrypted Databases via
  Adjustable Leakage," USENIX Security 2020. This supports the idea that
  practical encrypted databases often expose carefully defined adjustable
  leakage.
  <https://www.usenix.org/conference/usenixsecurity20/presentation/demertzis>

- Obladi: Crooks et al., "Obladi: Oblivious Serializable Transactions in the
  Cloud," OSDI 2018. This supports epoch/batch execution as a way to amortize
  oblivious storage costs.
  <https://www.usenix.org/conference/osdi18/presentation/crooks>

- Menhir: Reichert et al., "Menhir: An Oblivious Database with Protection
  against Access and Volume Pattern Leakage," ASIA CCS 2024. This is useful for
  discussing why aggregate volume or hit-count leakage should be stated
  explicitly rather than ignored.
  <https://www.hiig.de/publication/menhir-an-oblivious-database-with-protection-against-access-and-volume-pattern-leakage/>

- Trusted cloud DBMS survey/discussion: "Towards High-performance and Trusted
  Cloud DBMSs." This is useful for explaining why large versus constrained TEE
  memory is an important systems distinction for database workloads.
  <https://link.springer.com/article/10.1007/s13222-025-00495-8>

## Recommended Evaluation Framing

For experiments and text, keep the storage and execution axes separate:

1. Client-state axis:
   minimal state, minimal state plus hot index, TEE-resident state.

2. Trusted-memory axis for TEE:
   large trusted memory and constrained trusted memory.

3. Execution-leakage axis:
   FO, batch-level tier membership, and TM.

This avoids a common reviewer objection: a fast result under a large local cache
does not automatically imply a fast result when only a small hot index or stash
is available. It also lets SOMAP make a sharper claim: it can exploit skew under
different storage budgets, but the acceleration source and leakage profile differ
by regime.
