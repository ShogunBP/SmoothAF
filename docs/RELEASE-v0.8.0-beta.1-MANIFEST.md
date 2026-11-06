# SmoothAF v0.8.0-beta.1 Release Manifest

Status: prepared, not published.

## Source and release identity

| Field | Value |
|---|---|
| Release | `v0.8.0-beta.1` |
| Release type | Pre-release / Experimental |
| Source branch | `feature/af-algorithm` |
| Public release commit | The final documentation commit on `feature/af-algorithm` before the tag |
| Binary source commit | `31912f4d2e089b320b4664eac92ae665e4521f17` |
| Hygiene commit | `90a21851624a1474195607d49e44ad49aa8235a4` |
| Future tag commit | Must point to the final public release commit and be recorded after the local commit is created |
| Tag status | Not created |

The binary was built before the hygiene commit and before the final release-documentation commit. Its embedded Build ID therefore identifies the binary source commit, while the future release tag identifies the complete public source snapshot. These identities must not be conflated. At publication time, verify `git rev-parse HEAD` and require the tag to point to that exact final public commit before pushing either the branch or tag.

## Artifact

| Field | Value |
|---|---|
| File | `smoothaf.mo` |
| Build ID | `31912f4d2e08-600D.102` |
| SHA-256 | `494aa507f44dccf8e7403b0106cf7c6bfa8b91f5c0f36a13720c2d9708f28a4c` |
| Size | `131444` bytes |
| Target | Canon EOS 600D / Rebel T3i, firmware 1.0.2 |
| Legacy target | Magic Lantern Legacy Lua Fix |
| ARM dependency result | `DEP=39`, `Will load on: 600D` |

Only the locally preserved module with this exact identity may be attached to the release. Do not rebuild or replace it without repeating source, Build ID, size and SHA-256 verification.

## Publication boundary

The release must contain only:

- `smoothaf.mo` with the identity above;
- the public GitHub source at the future tag;
- release notes and public documentation.

Do not publish:

- physical camera logs;
- historical or experimental build backups;
- `600D_102.SYM`;
- the Legacy Lua Fix checkout;
- local ML trees, SD-card backups or credentials;
- unreviewed B2-B files.
