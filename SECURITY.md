# Security

Macha is a trusted shared-key cluster. Anyone with the cluster key can authenticate as a node and access cluster data. Do not give the key to an untrusted machine.

Generate at least 256 bits of random key material and protect the file:

```sh
openssl rand -hex 32 > /etc/macha.key
chmod 600 /etc/macha.key
```

The key is never sent over the network. Every node must nevertheless have the same key contents.

## Cryptography

- SHA-256 object IDs and plaintext integrity checks.
- HKDF-SHA256 domain-separated cluster-key derivation.
- HMAC-SHA256 authenticated transport handshake.
- Ephemeral X25519 Diffie-Hellman on every connection.
- HKDF-SHA256 directional session keys.
- AES-256-GCM transport frames.
- AES-256-GCM authoritative extents, cache objects and metadata at rest.
- Random 96-bit GCM nonces from OpenSSL `RAND_bytes`.

The handshake authenticates peer identity, the negotiated frame ceiling and the ephemeral X25519 exchange. Each peer pair uses one canonical bidirectional connection per transport lane: CONTROL, and DATA for object payloads. Variable-length frames are independently AES-256-GCM protected, and frame type decides a frame's transport priority. The exchange has forward secrecy against later disclosure of the shared cluster key, assuming the endpoint and ephemeral session secrets were not compromised while the session was live.

## Network exposure

The protocol is authenticated; still restrict it with the host/network firewall. A node is dialled at its advertised address unless it declares, or is found by a dial-back probe, to accept no inbound connections (`network.inbound_capable`); such a node dials its peers itself and is never dialled. Two nodes that both accept no inbound connections cannot reach each other. Optional UPnP IGD port mapping (`network.upnp`) is the only NAT assistance; there is no relay or anonymity layer.

For Internet deployment use routable addressing, explicit forwarding, or a private routed overlay such as WireGuard.

The catalogue/playback HTTP API is separate from the authenticated cluster protocol. It binds to loopback by default. If exposed beyond the local host, restrict it with the host/network firewall.

## HTTP accounts and roles

The shared cluster key authenticates *nodes*. It says nothing about *people*, and the HTTP API has its own identity model.

Every API route requires a session bearer token from `POST /api/v1/session`. The exceptions are that route itself, `GET /api/v1/health` (service name, readiness and running version only), the web client's static files when a web root is configured, and the capability URLs for streams and artwork described below. A session is minted either from a username and password, or -- when `session.allow_anonymous` is on -- with no credentials at all, in which case it is bound to the `anonymous` account. Each session carries the roles of the account behind it, and every route is gated on those roles in one place before dispatch.

Roles are capabilities rather than a ladder: `view_status` sees cluster health, `media_viewer` reads and plays media, `importer` acquires, `manager` changes files/namespaces/catalogue matches and resets cluster identity associations, `manage_users` administers accounts. `importer`, `manager` and `manage_users` each imply `media_viewer`, which implies `view_status`; nothing else implies anything. `view_status` is grantable alone, so health can be exposed without exposing media.

The node that founds a cluster creates `root` (every role) and `anonymous` (`media_viewer`) once, on first start, and writes root's generated password to `<state_path>/initial-root-password` with mode 0600. Read it, sign in, change the password, delete the file. Neither account can be renamed or deleted. Anonymous access is controlled by editing the `anonymous` account's roles, not by configuration -- this is what decides what an unauthenticated television can reach.

### Losing the root password

There is no recovery key and no recovery endpoint, deliberately. Reset the password on any node, with that node stopped:

```
macha-users <state_path> <cluster.key> passwd root
```

It replicates to the rest of the cluster when the node starts. An account holding `manage_users` can also reset it through the API without stopping anything.

The reasoning, since the absence is a decision rather than an omission: a recovery key would have to be presentable without an account to be useful, which means a standing unauthenticated path to the most privileged account in the cluster, on a surface that includes an offsite node. The only party who could present one is the operator, who already has root on a node — where the command above does the same job and needs no secret to have survived months in a drawer. Anyone able to use a recovery key could use `macha-users` instead, so the key adds exposure and no capability.

`macha-users` is not itself a weakness. It needs write access to the node's state directory and the cluster key to unseal it, which is root on a node — and per the trust model above, that party already has every byte in the cluster and can join it as a node. Creating themselves an account is a lateral move inside a compromise that is already total, the same way `passwd` is on any Unix host.

Passwords are scrypt-hashed with per-record parameters and never leave a node: no API route reads one back. The user table replicates to every node, including any offsite one, and is sealed at rest under a key derived from the cluster key. That is protection against a stolen disk, not against the cluster key: per the trust model above, anyone holding the key already has everything.

Verifying a password reads only the local node's replica, so authentication keeps working on a node that is partitioned or whose metadata has gone read-only. The corollary is that a partition admits account writes on both sides and resolves them last-write-wins, so a simultaneous change on both sides loses one. This is deliberate: refusing account changes without quorum would mean being unable to fix an account precisely when the cluster is unhealthy.

A password change, a role change or a deletion retires every session that account had minted, on every node, as the updated record propagates. That includes a recovery reset, so anyone signed in as root when it happens is signed out.

At least one account always holds `manage_users`. Removing the role from the last account that has it, or deleting that account, is refused — root included, whose roles are otherwise ordinary. The invariant is about the role rather than any particular account, so it moves as the role moves.

Playback control requests use the ordinary API Bearer token. A successful session returns a separate high-entropy capability in each stream URL because native media players cannot reliably attach the permanent API header to every playlist, fragment and range request. Treat the returned stream URL as a temporary bearer secret: anyone who has it can read that session's media until the session is deleted or expires. Capability URLs are scoped to one playback session and generated HLS generation; they do not authenticate cluster RPC or catalogue mutation. Catalogue artwork URLs likewise need no header: each carries an HMAC signature under a cluster-key-derived key and an expiry bucketed to `catalogue.api.artwork_capability_ttl_ms` (30 days by default), so anyone holding one can fetch that artwork until it expires.

Mixed protocol versions fail the handshake rather than downgrade, so a peer cannot be induced to speak an older, weaker version of the exchange.

## Key loss and compromise

Losing the cluster key makes encrypted cluster data unrecoverable. Back it up separately.

Compromise lets an attacker join the trusted cluster from that point onward. Macha currently has no online cluster-key rotation or per-node cryptographic revocation.

Forward secrecy protects old transport captures; it does not protect stored encrypted data from someone who later obtains both that data and the cluster key-derived storage key.

## Metadata safety

Metadata publication is gated on a durability floor, not a majority, so disconnected cohorts of `dht.metadata_min_write_replicas` nodes can each continue writing and produce divergent valid histories. That is an availability decision with a safety consequence: divergence is preserved and reconciled rather than resolved by discarding a branch. Incompatible alternatives become durable first-class conflicts, and no branch is silently chosen. See [Metadata replication and reconciliation](docs/metadata.md).

The operational consequence is that a floor well below the cluster size widens the window in which two cohorts write independently. Choose it for the failures you intend to survive, not for the largest number of nodes that can be lost.

Offline ancestry repair (`macha-metadata-repair`) is deliberately narrow: it refuses concurrent or equal clocks, ordinary mergeable histories and insufficient witnesses, so it cannot be used to pick a winner between two live branches.

Keep cluster policy identical on every node. Failure-domain labels affect placement only; they are trusted operator configuration, not a security boundary.

## Reporting

Use the repository's private security-reporting mechanism where available. Do not publish exploit details, real keys or private data in an issue.
