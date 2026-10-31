@AGENTS.md

## Mac (FreeTVC-Mac fork)
- Load the **freetvc-mac** skill (`.claude/skills/freetvc-mac/`); the state of the port: `MAC_ALLAPOT.md`.
- Talk to the user in Hungarian, briefly. The user never runs git: commit and push are Claude's job, to the `mac`
  remote (https://github.com/LaM3rzLG/FreeTVC-Mac), author and committer always `Deybacsi <deyprof@gmail.com>`.
- **Never run the automatic tests** (`ctest`); build with `bash tools/mac/build_mac.sh`.
- Update `MAC_ALLAPOT.md` after every meaningful change.
