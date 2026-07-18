# Security Policy

## Status

`sklad` is a young storage engine. Its core paths — durability (WAL), crash
recovery (WAL replay + MANIFEST), and compaction correctness — are covered by an
automated test suite, but it has not yet seen large-scale production use. Please
validate it against your own durability and corruption requirements before
trusting production data to it.

## Reporting a Vulnerability

Please do **not** open a public issue for security-sensitive problems (for
example, a way to corrupt a database or read out-of-bounds from a crafted
SSTable/WAL file).

Use the private GitHub Security Advisories channel (the **Security → Report a
vulnerability** tab in the repository). We aim to respond within 7 days.

## Scope

Untrusted database files are **not** a supported threat model: like SQLite or
LevelDB, `sklad` assumes the database directory it opens was written by itself
and has not been tampered with. Do not open database files from untrusted
sources.
