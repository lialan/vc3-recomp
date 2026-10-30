# vc3-recomp

Native macOS port of *Virtua Cop 3* (Sega Chihiro) by static recompilation with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). No game data is included:
you need your own Chihiro dump containing a decrypted `vc3.xbe` and `media/`.

See [docs/PLAN.md](docs/PLAN.md) for status and roadmap.

```bash
git submodule update --init
python3 -m venv .venv && .venv/bin/pip install capstone
brew install sdl2 libepoxy
VC3_GAME_DIR=/path/to/vc3 scripts/analyze.sh
```
