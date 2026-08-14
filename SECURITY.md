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

Transport v4 authenticates peer identity, the selected health/control/data lane and the ephemeral X25519 exchange. It has forward secrecy against later disclosure of the shared cluster key, assuming the endpoint and ephemeral session secrets were not compromised while the session was live.

## Network exposure

The protocol is authenticated; still restrict it with the host/network firewall. Peers must be directly reachable at their advertised address. The server has no NAT traversal or anonymity layer.

For Internet deployment use routable addressing, explicit forwarding, or a private routed overlay such as WireGuard.

Protocol v4 is intentionally incompatible with v3 and earlier. Mixed versions fail the handshake rather than downgrade.

## Key loss and compromise

Losing the cluster key makes encrypted cluster data unrecoverable. Back it up separately.

Compromise lets an attacker join the trusted cluster from that point onward. 0.4.0 has no online key rotation or per-node revocation.

Forward secrecy protects old transport captures; it does not protect stored encrypted data from someone who later obtains both that data and the cluster key-derived storage key.

## Metadata safety

A metadata minority refuses mutations rather than creating a second namespace history. Committed-checkpoint replacement recovery is deliberately constrained and does not turn recovery witnesses into voters.

Keep cluster policy identical on every node. Failure-domain labels affect placement only; they are trusted operator configuration, not a security boundary.

## Reporting

Use the repository's private security-reporting mechanism where available. Do not publish exploit details, real keys or private data in an issue.
