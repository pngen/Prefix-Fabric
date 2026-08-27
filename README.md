# Prefix Fabric 1.0.0

Prefix Fabric is a vendor-neutral runtime for indexing, matching, sharing, and governing reusable token-prefix structure across AI inference systems.

> **The core systems question:**
> What reusable prefix structure exists, which requests overlap with it, and how should that overlap be represented, discovered, shared, and reused?

## Why prefix structure deserves its own runtime boundary

Inference systems already share work along token prefixes. Two requests that begin with the same token sequence can reuse the state built for that shared prefix, and that reuse is the single largest source of latency and cost savings in token-repeat-heavy workloads. But the thing that makes a prefix reusable is not the state itself: it is the *structure* - the identity of the prefix, the compatibility context in which it means the same thing, the matching that decides whether a new request overlaps, the lineage of derived branches, and the metadata describing where reusable state lives.

That structure has its own lifecycle, its own consistency and fencing concerns, its own capacity pressure, and its own failure modes. It is shared by many requesters and by many unrelated state-owning subsystems. When prefix structure is scattered across engines or coupled to a particular state representation, the result is duplicated identity schemes, divergent compatibility rules, insecure or stale sharing, and no single place to observe or explain reuse decisions.

Prefix structure therefore deserves a dedicated, vendor-neutral runtime boundary:

- **Identity is universal.** The same logical prefix must produce the same identity across processes, engines, and nodes so that shared work can be named once.
- **Compatibility is explicit.** "Same tokens" is not enough; a prefix only means the same thing under the same tokenization, vocabulary, template, and layout. Compatibility must be a first-class, comparable notion rather than an implicit assumption.
- **Reuse is a discovery problem.** Given a request, the system needs to know deterministically which reusable prefix exists, how far the overlap goes, and what external state may be attached - independent of any one engine's cache.
- **Metadata must be separable from payload.** The structure is not the KV blocks or tensors. Owning structure and owning state are different responsibilities, and conflating them makes reuse fragile.

## The architecture boundary

> KV Fabric governs the reusable state. Prefix Fabric governs the reusable prefix structure that tells systems where shared inference work exists.

Prefix Fabric is the boundary that answers the core systems question: it represents, discovers, shares, and governs reusable prefix structure. It deliberately does not own, move, or interpret the reusable state that the structure points to.

## Prefix Fabric is NOT KV Fabric

> **Prefix Fabric is NOT KV Fabric.**
>
> Prefix Fabric governs the reusable prefix structure, identity, matching, lineage, and metadata that tell a system where shared inference work exists. It never governs the reusable-state payload itself. KV blocks, tensor state, buffers, and any other reusable computational state are owned entirely by the referencing subsystem. Prefix Fabric records opaque, backend-neutral StateReferences and metadata about them; it does not carry, allocate, persist, move, reclaim, restore, or interpret the bytes those references point to.

The separation is the point. Prefix Fabric answers *where* shared inference work exists and *how* to name and match it. KV Fabric (and the other state-owning runtimes below) answer *how that state is stored and reused.*

## Ownership at a glance

| Runtime | Owns |
| --- | --- |
| Prefix Fabric | token-prefix structure, indexing, matching, sharing, lineage, reuse discovery |
| KV Fabric | reusable KV inference state |
| Tensor Cache | reusable tensor-state caching |
| Context Fabric | arbitrary reusable computational state |
| Unified Buffer | heterogeneous buffer objects |
| Transfer Fabric | data movement |
| Topology Fabric | hardware topology/path cost |
| Memory Pressure | scarcity signaling |
| Compute Fabric | execution placement |
| Reclaim Fabric | reclamation economics |
| Checkpoint Fabric | execution survival |

## Architecture and design

### Identity model

Every known reusable prefix is identified by a stable, content-derived 128-bit `PrefixId`. The id is derived by taking the first 128 bits of a SHA-256 digest computed over a canonical serialization of the namespace, the compatibility identity, and the token sequence. A `PrefixId` is never a process-local pointer, a runtime handle, or a surrogate assigned by a particular engine. Under the same namespace, two identical logical prefixes produce the same `PrefixId` on every process and node. The id is scoped by the store's namespace, so the registry can be shaped like any other namespaced catalog.

### Compatibility model

Whether two token sequences are "the same" is never assumed: it is decided by an explicit `CompatibilityKey`. Two keys are equal if and only if every field is equal. The fields are `model`, `tokenizer`, `tokenization_version`, `vocabulary`, `special_token_policy`, `prompt_template`, `position_encoding`, `kv_layout`, `runtime_format`, plus an `extra` set of opaque axis key/value pairs (sorted for canonical ordering). Identical logical inputs - namespace + compatibility identity + token sequence - produce identical ids regardless of other runtime state. Prefix Fabric never infers compatibility; it only compares explicit keys. Omitting the axes that would conflate semantically different prefixes (`model` or `tokenizer`) is warned about as over-permissive, but the warning never changes matching behavior.

### Exact and longest-prefix matching

A request is matched against the structural index for its compatibility identity. `LongestPrefixMatch` returns the longest known active prefix that is a true prefix of the request, along with the matched prefix, the unmatched suffix, and whether the match was exact. `ExactMatch` returns a match only when an active prefix's full sequence equals the request. `AllMatches` returns every matching prefix from shortest to longest, and `CommonPrefixLength` reports the contiguously shared token count for two sequences. Matching never reports a match longer than the request and never returns an inactive node.

### Structural index

The index is a sharded collection of compressed radix tries over token sequences - one trie per (namespace, compatibility identity). Edges are labeled by token sequences and compressed so that a common path is stored once; interior nodes whose cumulative token count is nonzero are materializable prefixes with their own id, content hash, segments, and state references. The index is sharded (a fixed number of shards selected by hashing the compatibility identity), and within a trie the children are kept sorted so traversal, enumeration, and serialization are deterministic.

### Segmentation

A logical prefix may be represented as one or more canonical segments. Segmentation of the same logical prefix with the same policy is deterministic. Supported policies are `kSingle` (one segment covering the whole prefix), `kFixed` (fixed-size token blocks), `kCaller` (explicit caller-provided boundaries), and `kAdaptive` (common-prefix-aware). Each segment may carry an optional external StateReference.

### Branching lineage

Prefixes form a lineage graph of ancestors, descendants, and branches. `Path`, `Ancestors`, `Descendants`, `Children`, `BranchPoints`, and `CommonAncestors` expose the graph. Composition (`Compose`) and `Fork` derive new records from existing ones. The lineage is a DAG: it does not impose a single-parent requirement across the graph, and cycle-causing operations are rejected (`kCycleDetected`) rather than silently recursing. Garbage-collected or retired records are distinguished from current ones by generation, never silently overwritten.

### External StateReferences

A `StateReference` is an opaque, backend-neutral reference to reusable state owned by another subsystem (KV Fabric, Tensor Cache, Context Fabric, an engine-native cache, or application-defined state). Prefix Fabric records, attaches, retires, invalidates, and returns these references. It never allocates, moves, persists, reclaims, restores, or interprets the referenced bytes. The target object id is an opaque string interpreted only by the owning subsystem; Prefix Fabric carries enough metadata (namespace, object id, generation, freshness, integrity token) to reject stale references without knowing what the bytes are.

### Distributed directory

The directory is a coordinator with a set of nodes over framed TCP, supporting remote publish, lookup, invalidate, and attach. Every operation is fenced by a coordinator epoch and a per-node boot id. A node that starts a new boot identity invalidates prior authority from the same node, and any operation carrying a stale authority is rejected. Remote operations are bounded by the configured frame limit and then the configured capacity limits.

### Persistence

A store can be saved and reloaded atomically. Save writes to a temporary file, flushes it, and renames it into place; the serialized payload is checksummed (SHA-256), and corruption is rejected on load rather than partially applied. On recovery the store issues a new boot identity, so any authority that was minted before the recovery is stale and fails thereafter.

### Authority and fencing

Mutating authority is fenced at four levels: the coordinator epoch, the node boot id, the `PrefixId` generation, and the mutation generation. A stale authority cannot mutate. This makes partition and crash recovery safe: a node that has been fenced out, or a directory that has recovered, cannot issue mutations that would be misidentified as current.

### Invalidation

Invalidation is explicit, deterministic, and inspectable. A single prefix, an entire subtree, a whole compatibility identity (`InvalidateByCompat`), or all state references matching a state object can be invalidated. Invalidation marks records inactive and bumps their generation, so a subsequent lookup and a subsequent reuse plan both observe the change and never silently reuse retired state.

### Bounded capacity

Every runtime is bounded by an explicit, configurable `CapacityPolicy` (max prefixes, segments, token-index footprint, state references, total index nodes, serialized object size, network frame, subscriptions, and event backlog). Exceeding a bound is a recoverable error, never a silent unbounded allocation. Eviction is deterministic for a given state and configuration, and, when enabled, ancestors of live descendants are protected so that evicting a prefix never orphans the lineage that still depends on it.

### Thread safety

The index is genuinely thread-safe. Each shard has its own lock so that structural mutation is serialized per shard while readers on a compat tree run concurrently without taking a global lock. Callbacks never run while an internal lock is held, so reentrant subscribers cannot deadlock.

### Determinism

Determinism is a hard requirement. Serialization, ids, segmentation, longest-prefix choice, eviction order, candidate ranking, and snapshots are deterministic and never depend on unordered container iteration. Identical inputs under identical configuration produce identical output, which is what makes persistence, replay, and cross-node agreement possible.

### Observability

The runtime emits structured events (`PrefixEvent`) - added, matched, exact hit, longest-prefix hit, miss, branch created, segment created, state attached/retired, invalidation, eviction, recovery, ownership transfer, stale-authority rejection, persistence failure, corruption rejection, remote join/leave - through bounded subscriptions. Aggregate, observability-only counters are available as `RuntimeStatistics`. These never affect correctness.

### Explainability

Reuse decisions are inspectable. Reuse plans carry the matched prefix, the unmatched suffix, confidence, the compatibility identity used, the structural path, and per-candidate accept/reject reasons. Explain APIs and the `prefix-fabric explain` CLI provide a human-readable account of why a request matched (or did not) and which attached state was usable, stale, or invalid.

## Example

A tiny example showing add, longest-prefix match, and a reuse plan:

```cpp
namespace prefix_fabric {
  // A store in the default (empty) namespace and default options.
  PrefixStore store(PrefixStore::Options{});

  // Declare the compatibility context in which a prefix means the same thing.
  CompatibilityKey compat;
  compat.model = "example-model";
  compat.tokenizer = "example-tokenizer";
  compat.tokenization_version = "1";
  compat.vocabulary = "example-vocab";

  // Register a reusable prefix (5 tokens) in that compatibility context.
  auto rec = store.Add(compat, TokenSequence{1, 2, 3, 4, 5});

  // A longer request that begins with the same 5 tokens.
  TokenSequence req{1, 2, 3, 4, 5, 6, 7};

  // Longest-prefix match: finds the reusable prefix and the unused suffix.
  // Pass the compatibility key so we search the same tree we just added to.
  PrefixMatch m = store.LongestPrefixMatch(req, &compat);

  // Advisory reuse plan: describes where shared work exists and which
  // external state may be referenced (it does not execute inference).
  PrefixReusePlan plan = store.BuildReusePlan(req, &compat);
}
```

The matching entry points take an optional `CompatibilityKey` pointer; when omitted they use the store's `default_compat`. The core API surface is `Add`, `LongestPrefixMatch`, `ExactMatch`, `AllMatches`, `BuildReusePlan`, `CommonPrefixLength`, `Get`, `Path`, `Ancestors`, `Descendants`, `Children`, `BranchPoints`, `CommonAncestors`, `Segments`, `Statistics`, `Slice`, `Append`, `Compose`, `Fork`, `AttachState`, `RetireState`, `InvalidateStateByObject`, `Invalidate`, `InvalidateByCompat`, `Remove`, `Evict`, and `SaveToFile`/`LoadFromFile`.

## Limitations

Prefix Fabric owns metadata and structure only. The referenced payload (KV blocks, tensors, buffers, and other reusable computational state) is owned by the referencing subsystem, and Prefix Fabric never further touches it. Prefix Fabric:

- executes no inference; a `PrefixReusePlan` is an advisory description of where shared work exists, not a computation;
- requires and integrates none of KV Fabric, Tensor Cache, Context Fabric, vLLM, SGLang, TensorRT-LLM, llama.cpp, PyTorch, CUDA, or ROCm; those are represented only through opaque `StateReference` seams;
- does not depend on any ML or GPU library.

Linux is supported through portable seams in the public API, but this release is validated on Windows 11 x64 only. Builds on other platforms are not validated in this release.

## Build and install

Configure, build, test, and install from the repository root:

```sh
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix <install-dir>
```

The install exports a CMake package, so downstream consumers can link it with:

```cmake
find_package(prefix_fabric CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE prefix_fabric::prefix_fabric)
```

CMake options that gate optional targets are `PREFIX_FABRIC_BUILD_TESTS`, `PREFIX_FABRIC_BUILD_EXAMPLES`, `PREFIX_FABRIC_BUILD_BENCHMARKS`, `PREFIX_FABRIC_BUILD_CLI`, and `PREFIX_FABRIC_WARNINGS_AS_ERRORS`.

## Requirements

- C++20.
- CMake 3.20 or newer.
- MSVC (Windows 11 x64 is the validated platform for this release).
- No ML or GPU dependency.
- No third-party runtime dependency (SHA-256 and the binary serialization are self-contained).

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
