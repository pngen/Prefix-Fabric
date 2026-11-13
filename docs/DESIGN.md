# Prefix Fabric Design

This document describes the design of Prefix Fabric 1.0.0: the identity and compatibility model, canonical hashing, the compressed radix trie and its edge-splitting algorithm, sharding and concurrency, the binary serialization format, the atomic persistence protocol, the fencing/epoch model, the eviction scoring, and the event hub.

Prefix Fabric is a vendor-neutral runtime for indexing, matching, sharing, and governing reusable token-prefix structure. It owns metadata and structure only. It never owns the payload referenced by a `StateReference`.

## Identity model

A reusable prefix is identified by a stable 128-bit `PrefixId`. It is derived, not assigned. The derivation is:

- Build the canonical bytes for the logical prefix: `CanonicalPrefixBytes(ns, compat, tokens)`.
- Hash those bytes with SHA-256 to get a 256-bit `ContentHash`.
- Take the first 128 bits of that digest (the high word is the first 8 bytes, the low word the next 8 bytes) as the `PrefixId`.

Because the id is a function of the logical content, two identical logical prefixes under the same namespace and compatibility identity - no matter which process, node, or engine created them - produce the same `PrefixId`. The id is never a process-local pointer, a runtime handle, or a surrogate minted by any particular engine. Namespace scoping mirrors the namespacing of a registry, so the same token sequence can be tracked separately in different stores.

A `SegmentId` (the identity of a single segment) is the same 128-bit type and is derived the same way from the segment's canonical bytes.

## Compatibility model

"Same tokens" is not a sufficient basis for reuse: tokens are only semantically identical in a context that makes them identical. The `CompatibilityKey` makes that context explicit and comparable. Two keys are equal if and only if every field is equal:

- `model`
- `tokenizer`
- `tokenization_version`
- `vocabulary`
- `special_token_policy`
- `prompt_template`
- `position_encoding`
- `kv_layout`
- `runtime_format`
- `extra` - opaque extra axes as key/value pairs, sorted for canonical ordering

The key exposes a deterministic textual `Identity()` (a fixed field order with sorted `extra` pairs) and an `IdentityHash()` that is SHA-256 over that identity string. Prefix Fabric never infers compatibility; it only compares explicit keys. If a key omits the axes that would let semantically distinct token sequences be conflated - in particular `model` or `tokenizer` - `IsOverPermissive()` warns loudly. That warning is advisory; it never changes matching behavior, because Prefix Fabric deliberately does not guess what the caller meant.

Identical logical inputs (namespace + compatibility identity + token sequence) therefore produce identical ids. That is the invariant that makes cross-process sharing, persistence, and cross-node agreement possible.

## Canonical hashing rationale

The content hash is SHA-256 over the full canonical bytes of the prefix. Those canonical bytes are length-prefixed and deterministic:

- `AppendStr(ns)` - the namespace, length-prefixed.
- `AppendStr(compat.Identity())` - the compatibility identity string, length-prefixed.
- `AppendU64(tokens.size())` - the token count.
- For each token, `AppendU64(token)` (token ids are stored as 64-bit in the canonical bytes).

Length-prefixing makes the encoding unambiguous - there is no way for two distinct inputs to serialize to the same byte string, so the hash is collision-resistant against input ambiguity and not merely against byte collisions.

The hash is the full 256-bit digest, and it is used for identity without truncation to a weak checksum. Equality between a stored prefix and a request is decided by the full canonical bytes or the full content hash, not by a shortened checksum that could equate two different prefixes. Truncating the digest could make two different logical prefixes equal by accident, which would corrupt reuse and lineage. Prefix Fabric does not do that: `PrefixId` truncates the SHA-256 digest only to fit the 128-bit identity namespace, but the content equality signal retained in the index (the `content_hash` and the canonical token sequence) is never reduced to a weak checksum for equality decisions.

## Structural index

The index is a sharded collection of compressed radix tries, one trie per (namespace, compatibility identity). Each edge is labeled by a token sequence and is compressed: a run of tokens with no branching is stored on a single edge rather than as one node per token. Interior nodes whose cumulative token count is nonzero are materializable prefixes; each such node carries its own `PrefixId`, content hash, token count, generation, active flag, depth, provenance, segments, and state references.

Children of a node are kept sorted by edge label. This keeps traversal, child enumeration, and serialization deterministic and makes the preorder order stable.

### Compressed radix trie and edge splitting

The trie is a radix tree over token ids. A node's edge is the token string that connects it to its parent. The invariant is that each node's `token_count` is the cumulative length of the full path from the root, and the node is a shared prefix for everything in its subtree.

**Insert.** Walk from the root consuming the request tokens along matching edges. When an edge is fully consumed, move to that child and continue. When no child matches the next token, create a new child whose edge is the remaining request suffix, insert it in sorted order, finalize it, and return. If the request is fully consumed at an existing node, bump its generation, attach any state references, recompute segments, and return that node.

**Edge split.** When a request shares only a prefix of a child edge (the child edge starts with the next token but the common prefix is shorter than the whole edge), the edge must be split. Detach the child from its parent, create a new split node whose edge is the common prefix, re-parent the child below the split node with the remaining portion of the old edge, insert the child into the split node and the split node into the parent (both in sorted order), finalize the split node, continue the walk from the split node. The compaction property is preserved: the split node becomes a new branching point that represents a real shared prefix.

**Longest-prefix match.** Walk from the root consuming the request along edges. Descend into a child only when the child edge is entirely consumed by the request (the common prefix of the child edge and the remaining request equals the full child edge). When the request runs out, or when a child edge is only partially matched, stop - the walk never reports a match longer than the request. Then climb from the stopping node to the nearest active node with nonzero token count. The matched prefix is that node's full sequence up to its token count; the unmatched suffix is the remainder of the request; the match is exact only if the matched token count equals the request length.

Concise pseudocode:

```
function CommonPrefix(a, a_off, b, b_off):
    n = 0
    while a_off+n < a.size and b_off+n < b.size and a[a_off+n] == b[b_off+n]:
        n += 1
    return n

function TrieInsert(t, tokens, refs, provenance):
    if tokens.empty:
        return t.root
    node = t.root
    i = 0
    loop:
        if i == tokens.size():
            node.generation += 1
            attach(refs, node)
            node.provenance = provenance if nonempty
            node.segments = ComputeSegments(t.compat, Full(node))
            return node
        child = the child of node whose edge starts with tokens[i]
        if child is none:
            c = new node with edge = tokens[i..], parent = node, depth = node.depth+1
            insert c into node.children (sorted)
            FinalizeNew(t, c, refs, provenance)
            return c
        lcp = CommonPrefix(child.edge, 0, tokens, i)
        if lcp == child.edge.size():
            node = child
            i += lcp
            continue
        # edge split
        detach child from node.children
        split = new node with edge = child.edge[0..lcp), parent = node, depth = node.depth+1
        child.edge = child.edge[lcp..)
        child.parent = split
        child.depth = split.depth + 1
        insert child into split.children (sorted)
        insert split into node.children (sorted)
        FinalizeNew(t, split, refs if i+lcp == tokens.size() else none, provenance)
        node = split
        i += lcp

function Walk(t, request):
    node = t.root
    i = 0
    while i < request.size():
        child = the child of node whose edge starts with request[i]
        if child is none: break
        lcp = CommonPrefix(child.edge, 0, request, i)
        if lcp != child.edge.size(): break   # do not report a match longer than request
        i += lcp
        node = child
    return (node, i)

function LongestPrefixMatch(t, request):
    (node, i) = Walk(t, request)
    while node is not none and (not node.active or node.token_count == 0):
        node = node.parent
    if node is none or node.token_count == 0:
        return none
    full = Full(node)
    matched_prefix = full[0..node.token_count)
    unmatched_suffix = request[node.token_count..)
    exact = (node.token_count == request.size())
    return PrefixMatch(node.id, node.token_count, matched_prefix, unmatched_suffix, exact)
```

## Sharding and concurrency

The index is split into a fixed number of shards. A compatibility identity maps to a shard by hashing its `IdentityHex()` string and taking the remainder modulo the shard count. A trie always lives in exactly one shard, so all operations on a particular compatibility identity contend on a single shard lock, and distinct identities usually run on distinct shards.

Each shard owns a mutex. Two locks exist:

- A per-shard mutex, which serializes structural mutation of the tries in that shard.
- A single `shared_mutex` (`id_mtx_`) guarding the id-to-location map (`id_index_`) that maps a `PrefixId` to its shard and trie key.

**Lock ordering is a contract.** The directed order is: shard lock first, then the id mutex. Concretely:

- Mutation (`Insert`) takes the shard lock, and only while it is held does it take the id mutex (in `FinalizeNew`) to publish the new node's id.
- Lookup-first operations (`Get`, `Path`, lineage, `Invalidate`, `Remove`) must first resolve the id to a location. That lookup takes the id mutex, copies out the (shard, key) pair, and releases the id mutex before the caller acquires the shard lock.

So the id mutex is acquired only after the relevant shard locks, never before, and a shard lock is never acquired while the id mutex is held. This ordering removes the id-mutex-vs-shard-lock cycle that would otherwise deadlock. Reader concurrency on a compat tree is possible because a lookup takes the id mutex only briefly and read-style operations on a tree that do not need the id map run under a single shard lock with no global lock.

Callbacks are never invoked under any internal lock. Subscribers to events run on a dedicated dispatch thread after the relevant lock has been released (see below), so a subscriber may re-enter the store without deadlocking.

## Binary serialization format

A serialized store is a single byte blob. The container starts with an ASCII magic and a container version:

- Magic: `PFSX` (4 bytes, raw).
- Container version: `U32` = 1, little-endian.

The remainder is the checksummed payload. The payload is laid out as:

- Payload format: `U32` = 1.
- Tree count: `U64`.
- For each tree, sorted by compatibility identity hex:
  - The `CompatibilityKey` (all nine string fields as length-prefixed strings, then the number of `extra` pairs as `U64`, then each pair as two length-prefixed strings).
  - Node count: `U64`.
  - The tree's nodes in deterministic preorder (root first, then children sorted by edge label, visited depth-first). Each node is:
    - Parent index: `U64` (the index of the parent in the preorder array; the root uses `0xFFFFFFFFFFFFFFFF` to mean "no parent").
    - Edge token count: `U64`, then each edge token as `U32`.
    - `PrefixId` high word: `U64`.
    - `PrefixId` low word: `U64`.
    - Content hash: 32 raw bytes.
    - Token count: `U64`.
    - Generation: `U64`.
    - Active flag: `U8` (1 = active).
    - Depth: `U32`.
    - Provenance: length-prefixed string.
    - Segment count: `U64`, then each segment (id high/low, token offset, token count, 32-byte content hash, a state-ref presence flag and optional state ref, generation, active flag).
    - State-reference count: `U64`, then each reference (namespace, object id, generation, type enum, its own `CompatibilityKey`, location hint, optional freshness timestamp flag and value, integrity token).

After the payload comes the checksum: a 32-byte SHA-256 digest of everything before it (the whole container including magic and version, plus the payload). The reader:

- Rejects a blob that is too small (< magic size + 32) as corruption.
- Rejects a blob whose first four bytes are not `PFSX` ("bad magic") as corruption.
- Splits off the trailing 32-byte digest, recomputes SHA-256 over the remaining body, and rejects a mismatch as corruption - corruption is never partially applied.
- Parses the payload format and rejects an unsupported version.

All integer fields are little-endian, and all reads are bounds-checked through the `BinaryReader`: truncated or out-of-range reads return a corruption/invalid-argument error rather than crashing.

## Atomic persistence protocol

Saving is atomic. `WriteFileAtomic` writes the serialized blob to a sibling temporary file (`<path>.tmp`), flushes it to stable storage, and then renames it over the target path with replace semantics. On Windows it uses `CreateFileW` + `WriteFile` + `FlushFileBuffers` + `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. On non-Windows builds it uses a std `ofstream`, `flush()`, and `std::filesystem::rename`. The rename is the commit point: either the old state remains, or the new state is present; there is no window in which the target path holds a partial file. A crash before the rename leaves the old state intact.

Loading reads the whole file and deserializes it, replacing the current index state. Because the blob is checksummed and bounds-checked, a truncated, corrupted, or version-mismatched file is rejected as `kCorruption` / `kSerializationError` rather than partially applied. On recovery the store issues a new boot identity; any authority minted before the recovery (for example, a stale node handle under the old boot id) is stale and fails thereafter.

## Fencing and epoch model

Mutating authority is fenced at four levels so that crash and partition recovery never lets stale state be mistaken for current:

- **Coordinator epoch.** The directory coordinator advances a monotonic epoch on each change of authority. Operations are valid only for the current epoch; an operation that carries an older epoch is rejected.
- **Node boot id.** Each node gets a fresh boot id when it (re)starts. A node that starts a new boot identity invalidates any authority it held under a prior boot id. This makes recovery safe: a resurrected node cannot keep mutating as though nothing happened.
- **PrefixId generation.** Each prefix record and each segment carries a generation. Retired or invalidated records are distinguished from current ones by generation (bumped on invalidation), never silently overwritten.
- **Mutation generation.** A stale mutation generation is rejected, so a replayed or delayed mutation cannot be applied to a structure that has since changed.

The net effect is that stale authority cannot mutate. Fencing is what allows the distributed directory and the persistence recovery path to be safe under failures without stopping the world.

## Bounded capacity and eviction

Every runtime is bounded by an explicit, configurable `CapacityPolicy`: max prefixes, max segments, max token-index footprint, max state references, max total index nodes, max serialized object size, max network frame, max subscriptions, and max event backlog. Exceeding a bound is a recoverable `Result` error, never a silent unbounded allocation or a coercion of invalid input.

Eviction is deterministic for a given state and configuration. The index keeps a logical access clock (`access_clock_`) that advances on each access, and per-prefix statistics record lookup counts, exact/partial hit counts, reused tokens, branch fanout, descendant count, attached-state counts, and the last access time. The eviction score is a weighted composite of recency (from the logical access clock), frequency (from hit counts), and structural value (how many live descendants share this node). A pure recency-based policy can also be selected, as can a frequency-only or structural-value-only policy.

Crucially, the access clock is a logical, monotonically increasing counter rather than a wall-clock timestamp. This makes the recency component reproducible given the same operation sequence, so the eviction order is deterministic and replayable - essential for cross-node agreement and deterministic persistence. When protection is enabled, ancestors of live descendants are never evicted ahead of their dependents, so evicting a prefix never orphans a lineage that still references it. Optional auto-eviction lets an insert evict enough entries to stay within capacity instead of failing.

## Event hub

The store exposes structured events through a bounded event hub. Subscribers register a callback plus an optional filter of `EventType`. The hub keeps a bounded queue (dropping the oldest event when the backlog cap is reached) and a monotonic per-event sequence number and timestamp.

Event delivery does not happen on the calling thread under a lock. A dedicated dispatch thread pulls batches of events from the queue, snapshots the current subscriptions, and invokes each matching callback outside any internal lock. The callback therefore cannot deadlock even if it re-enters the store. A callback failure is caught and swallowed so that one broken subscriber cannot corrupt the runtime.

The event types observed are: `kPrefixAdded`, `kPrefixMatched`, `kExactHit`, `kLongestPrefixHit`, `kMiss`, `kBranchCreated`, `kSegmentCreated`, `kStateReferenceAttached`, `kStateReferenceRetired`, `kInvalidation`, `kEviction`, `kRecovery`, `kOwnershipTransfer`, `kStaleAuthorityRejection`, `kPersistenceFailure`, `kCorruptionRejection`, `kRemoteNodeJoin`, and `kRemoteNodeLeave`.

Events are observability only; they never affect correctness. Aggregate, similarly observability-only counters are available through `RuntimeStatistics` (lookups, hits, exact/partial hits, reused tokens, invalidations, evictions, reference attachments/retirements, stale-authority rejections, persistence failures, corruption rejections, and cycle rejections).
