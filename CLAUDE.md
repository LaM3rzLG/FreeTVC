@AGENTS.md

## Mac
- Load the **freetvc-mac** skill (`.claude/skills/freetvc-mac/`); the state of the port: `MAC_ALLAPOT.md`.
- Talk to the user in Hungarian, briefly. The user never runs git: commit and push are Claude's job, to `origin`
  (https://github.com/LaM3rzLG/FreeTVC, the only repo; FreeTVC-Mac is no longer used, no new repos), author and committer the user: `LaM3rzLG <lorinczg77@gmail.com>` (repo git config; from 2026-11-01, older commits are Deybacsi's).
- **Never run the automatic tests** (`ctest`); build with `bash tools/mac/build_mac.sh`.
- Update `MAC_ALLAPOT.md` after every meaningful Mac change; plans and GitHub issue status (not Mac-specific): `TERVEK.md`.
