@AGENTS.md

## Mac
- Load the **freetvc-mac** skill (`.claude/skills/freetvc-mac/`); the state of the port: `MAC_ALLAPOT.md`.
- Talk to the user in Hungarian, briefly. The user never runs git: commit and push are Claude's job, to `origin`
  (https://github.com/LaM3rzLG/FreeTVC, the only repo; FreeTVC-Mac is no longer used, no new repos), author and committer always `Deybacsi <deyprof@gmail.com>`.
- **Never run the automatic tests** (`ctest`); build with `bash tools/mac/build_mac.sh`.
- Update `MAC_ALLAPOT.md` after every meaningful change.
