---
name: protocol-change
description: Add or change a LocalGrid protocol message, body layout, routing rule, or lg_core behavior. Use when touching components/lg_core, the envelope, message types, presence, delivery states, or the time rule.
---

# Protocol change

The protocol core is shared by every node and handheld, so a change lands everywhere at once. Work in this order.

## Steps

1. **Check the decision.** Find the governing entry in `docs/DECISIONS.md` and the design review answer it cites (envelope: 17, IDs: 18, routing: 20–23, acks: 24, security: 26). If the change contradicts a decision, stop and ask the owner. Done when you can name the decision and answer the change conforms to.
2. **Wire format.** Message types live in `lg_envelope.h`; fixed body layouts and their encode/decode in `lg_body.h` and `lg_body.c`. Keep bodies little-endian with exact-length decoders that reject any other length. Unknown types must stay ignorable by older code. Done when every new field has a size, an offset, and a decoder check.
3. **Behavior.** Node logic goes in `lg_node.c`; handheld logic in `lg_client.c`. New side effects reach firmware through an `io` callback, never a platform call. Every new table has a fixed size from `lg_types.h`. Done when no new code path allocates or blocks.
4. **Tests.** Add scenarios to `tests/target/main/test_messaging.c` using the simulator in `sim.c`: the normal path, a duplicate delivered twice, a two-hop chain, and each rejection the change introduces. Done when every branch you added has a check that would fail if the branch were removed.
5. **Run.** Use the `bench` skill: build `tests/target` warning-free and get `LG_TESTS_RESULT: PASS` on a board. Rebuild every firmware project that includes `lg_core`.
6. **Record.** Add a `CHANGELOG.md` entry. If the wire format changed, update the design review answer that defines it.

## Invariants to preserve

- Message identity is (origin_id, origin_boot, origin_seq); wall-clock time is never part of identity, nonces, or dedup.
- A node validates before it marks a message seen, so a rejected message can be retried; a duplicate of an accepted message gets `ACCEPTED` again.
- 1:1 bodies stay opaque to nodes: nodes check only length and the `LG_FLAG_E2E_PAYLOAD` flag. Anything nodes rewrite (ttl, origin_node, RELAYED) stays out of `lg_e2e_aad`.
- A message id is always sealed with the same grid time and plaintext.
