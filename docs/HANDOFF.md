# Handoff: Galar Native, Arceus Native and Paldea Native

State as of 2026-11-19. This replaces the 2026-11-04 / 11-09 handoff. Everything below was read from the git history, the open pull
requests and the GitHub issues on that date. **The phone's state was not checked for this update**, and none of the unreleased branches below
has a recorded phone test unless it says so. All of the owner's testing so far was on one phone: a Galaxy S25+ (Snapdragon 8 Elite, Adreno 830).

**The bar:** a locked 30 fps is the minimum, on every supported phone, in gameplay (menus don't count).

## Taking over: start here

1. **Get the code.** One repository with three remotes. The main checkout is `C:\Users\daniel\PKShield-Android`; most branches have their own
   worktree under `C:\Users\daniel\` (`git worktree list`).
   ```
   git clone https://github.com/simipoB/Galar-Native-Android.git   # origin
   git remote add arceusrepo https://github.com/simipoB/Arceus-Native-Android.git
   git remote add paldea https://github.com/simipoB/Paldea-Native-Android.git   # private: ask the owner for access
   ```
2. **Branch model.** One engine, three apps:
   - **Galar**: feature branch → PR → a `release-0xx` branch that merges the PRs for that release → `origin/main`, tagged `v0.xx`.
   - **Arceus**: `arceusrepo/main` is Galar's tree plus the Arceus profile and Arceus-only work. Shared fixes are written once, then put on
     both sides as twin branches (for example `galar-lse-swap-emu` / `lse-swap-emu`, `galar-vulkan10` / `arceus-vulkan10`). Each app keeps its own README.
   - **Paldea**: behind both. It is brought up to a Galar release in one sync branch (`paldea-sync-081`). Its old `violet` branch (2026-11-06) is no longer used.
3. **Build.** See [BUILDING.md](BUILDING.md), or `GAME=<title id> bash scripts/build_apk_pc.sh`. One engine per game pair: `01008DB008C2C000` for
   Galar, `01001F5010DFA000` for Arceus, `01008F6008C5E000` for Paldea.
4. **Game data.** You need your own dumps and `prod.keys`; none are in the repo. Ask the owner for file paths: do not scan their Downloads.
5. **Read "Release status"** before publishing anything. Nothing goes out until it has passed on a phone and the owner says so.
6. **Measure before changing performance code:** `DM_VK_PROF=1`, `DM_VK_GPU_TIME=1`, now also `DM_VK_PASS_TIMES` (branch `pass-times`), and read
   `/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq` and `cpu6` with every result. A phone that is warm and charging over USB caps every core at 1.7-2.2 GHz
   (normal 3.53 / 4.47) even at thermal status 0. Use adb over Wi-Fi (`adb tcpip 5555`, `adb connect <ip>:5555`) and unplug before measuring.
7. **Claim a task in "Who is on what"** before you start, then work through the "Roadmap" in order. "Open problems" has the details.
8. **Update this file** when you finish or rule something out. Write down what you measured.

## Working in tandem (two agents: Claude Code and Antigravity)

Two coding agents work on this tree at the same time. Rules so that they don't undo each other's work:

1. **One worktree per agent per task**, never the same checkout: `git worktree add ../<task> -b <prefix>/<task> origin/main` (or `arceusrepo/main`).
   Branch prefixes: `cc/` for Claude Code, `ag/` for Antigravity. Don't commit to, rebase or force-push the other agent's branches.
2. **Claim before you start.** Add a row to "Who is on what" (task, agent, branch, files you expect to touch) and push that change to
   `handoff` (a branch for nothing but this file) so the other agent sees it. Remove the row when the PR is merged or the task is dropped.
3. **Hot files have one owner at a time.** `dm/gfx/nvn/nvn_vk.c` (about everything Vulkan), `dm/android/.../GameActivity.java` / `Settings.java`,
   `dm/android/android_main.c`, `setup_game.py` and `assets/lang/*.tsv`. If your task needs a hot file another agent has claimed, take a
   different task or wait for their PR. For `.tsv` files, only add lines at the end.
4. **Small PRs, rebased on `main` just before opening.** One problem per PR, with what was measured and on which phone. The owner merges; agents never
   merge into `main`, make release branches, tag or publish.
5. **Twins**: a shared engine fix is made by one agent on both sides (`galar-x` and `x` for Arceus), not split between agents.
6. **Phone time is shared.** Only one agent drives adb at a time: note it in "Who is on what" (`phone: <agent>`) and remove it when done.
   Never leave `env.txt` switches behind.
7. **Write results here, not only in chat.** The other agent cannot see your conversation.

### Who is on what

| Task | Agent | Branch | Hot files claimed | Since |
|---|---|---|---|---|
| _(empty: claim a roadmap item)_ | | | | |

Suggested split (the owner decides): **Claude Code** takes the engine lane (roadmap 1-3: heat / frame drops, the Dynamax crash, Mali, everything in
`nvn_vk.c` and `dm/host`). **Antigravity** takes the app and sync lane (roadmap 4-6: the Paldea sync, the Arceus release, the Java UI, touch issues,
issue triage). The lanes touch different hot files, so they rarely conflict.

## Roadmap

Ordered. Each step ends with a phone test on the S25+ and the owner's go-ahead.

**Now (this week)**
1. **Heat and frame drops** (problem 1). Measure v0.81 against v0.94 on the same cool, unplugged phone; find which change costs heat; fix or turn it
   off. Merge `perf-report` into Galar so players can send a Detailed performance log.
2. **Debug reports first, then crash hunting.** Phone-test and merge #318 `debug-bundle` (and Arceus #77) and #314 `dynamax-heap-id`, then use the
   reports for the Dynamax crash (problem 2) and Arceus's battle crashes (#79, #80).
3. **Mali** (problem 3): ship `b10-swap-audit` and `mali-vertex-formats` as menu tests, collect reports from Mali players, fix what they name.

**Next**
4. **Arceus v0.45**: phone-test `release-045`, add `capture-button` and `gyro-throw` if they pass, release.
5. **Galar v0.95**: one release branch for the waiting PRs (#313, #312 + #309, #307, #306, #302, #271), each phone-tested; only opt-in switches go in
   switched off.
6. **Paldea**: phone-test and merge PR #1 (v0.82), then sync from Galar `main` and release a first Paldea build that has run on a phone (problem 5).

**Later**
7. Arceus: purple shadows (problem 4), 1440p (`arceus-1440-pool`).
8. Re-enable, one at a time and measured: background pipeline builds, frame generation, Faster rendering on Galar, pipeline libraries by default.
9. Requested features: Arceus UI matching Scarlet / Violet's, fill-the-screen picture for Scarlet / Violet, cheats.
10. Test on a second phone (a Mali one first).

## Release status

| App | Latest release | Package | Repo |
|---|---|---|---|
| Galar Native (Sword, Shield) | **v0.94**, 2026-11-15 (Latest) | `com.nxnative.pokemonshield` | `simipoB/Galar-Native-Android` (public) |
| Arceus Native (Legends: Arceus) | **v0.44**, 2026-11-15 (Latest) | `com.nxnative.pokemonlegendsarceus` | `simipoB/Arceus-Native-Android` (public) |
| Paldea Native (Scarlet, Violet) | **v0.82**, 2026-11-13 (pre-release, never run on a phone) | `com.nxnative.pokemonviolet` | `simipoB/Paldea-Native-Android` (private) |

- Galar went v0.41 → v0.94 and Arceus v0.2x → v0.44 between 2026-11-09 and 11-15, often several releases a day. Nothing was released after 11-15.
- **Paldea v0.82 is not on `paldea/main`.** It was tagged on `paldea/paldea-sync-081` (PR #1, still open). `paldea/main` is v0.60. v0.82 has
  Galar v0.81's engine, so it is now **90 commits behind Galar `main`**.
- **Next Arceus release is staged:** `release-045` merges `arceus-late-wake`, `shader-override` (with `pass-times`), `vulkan11-notice`,
  `arceus-adreno-rp`, `pipeline-libraries`, `lse-swap-emu`, `shared-fonts` and `perf-report`. Not released, no recorded phone test.
- **Galar has no release branch after v0.94.** The PRs waiting for one are listed under "Open pull requests".
- Before publishing: install each release build over the phone's test build and boot every game of that app. On Galar's release page, delete old
  releases except the last 2 (keep the tags). Signed with the release key (see the rules), not debuggable.

## Where things are

| | |
|---|---|
| Game profiles | Shield `dm/games/01008DB008C2C000`, Sword `0100ABF008968000`, Arceus `01001F5010DFA000`, Violet `01008F6008C5E000`, Scarlet `0100A3D008C5C000` |
| Compatible devices | [DEVICES.md](DEVICES.md) (Sword / Shield) |
| Download site | `site/`, a Cloudflare Worker with a Turnstile check in front of the APK (Galar only) |
| Handoff files | [docs/handoff/](handoff/): older Arceus patches and the test scripts (`arun.sh`, `crowdtest.sh`, `flick2.py`, ...) |
| Debug report reader | `dm/android/tools/read_debug_report.py` (branch `debug-bundle`, PR #318 / Arceus #77) |
| Shader tools | `shader_override.py` (branch `shader-override`): get a shader out of the pack, build an edited one (`spirv-as` + `spirv-val`) |
| Old checkouts | `C:\Users\daniel\PokemonShieldAndroid` (to 2026-10-27) and `androidswithy` (the nxnative engine, Arceus's origin `84fb5df`) are history: do not work there |

Rules for this project:

- No AI co-author lines in commits or PRs.
- Sign releases with `dm/android/probe/out/debug.keystore` from the `PKShield-Android` checkout (cert `a5f49b44…`). It exists only on the owner's PC.
- Do not install over, or change, the owner's `com.nxnative.shield` and `com.nxnative.arceus` (older personal builds).
- Do not scan the owner's Downloads. Ask for paths.
- **Check the foreground app before every adb input.** The owner uses the phone between tests; input lands in whatever is on screen.
- The phone has about 4 GB free.
- License: GPL-3.0-or-later for each app as a whole (since 2026-11-10); the older code keeps its MIT notice. Credit borrowed code in the commit
  (for example "found by LZA-Recomp, MIT"), and describe borrowed performance ideas in comments without naming the source.

## Build and run

```
cd dm/android && python tools/win_build_lib.py <GAME_ID>
cp out/host-<GAME_ID>-vulkan/lib/libnxnative.so <libs folder>/
cd ../.. && STRICT_16K=1 python dm/android/tools/build_apk.py --game <GAME_ID> --version <n> \
  --libs <libs folder> --pylib <...>/aot/pyandroid/prefix/lib/python3.14 \
  --capstone-py <...>/aot/capsrc/capstone-5.0.7/bindings/python/capstone \
  --ks <keystore> [--debuggable] --out <file>.apk
```

- Or `GAME=<GAME_ID> bash scripts/build_apk_pc.sh` (all 8 steps).
- From Git Bash, give Python and adb `C:/...` paths and set `MSYS_NO_PATHCONV=1`.
- `DM_LTO=1` builds the engine with ThinLTO into its own folder: an experiment, its gain has not been measured.
- The NSP extractor for Android is built in WSL: `bash dm/android/extract_aot/build_extract_aot.sh` → `libnx_extract.so` (into the libs folder).
- Built-in code mods: `python dm/tools/builtin_mods.py <decompressed main> dm/host/builtin_mods.h name=<pchtxt>[,<extra build id>] ...`.
- Debug switches go in the game's `env.txt` (`Android/data/<package>/files/[<title id>/]env.txt`) and persist between runs. **Clear them after testing.**
  env.txt beats both Auto and the menu.
- `dm/android/tools/lang_src.py` is behind the committed `assets/lang/*.tsv`: running it would drop texts. Add texts to the `.tsv` files.

## What changed since the last handoff (2026-11-09 to 11-15)

Shared engine (all three apps unless stated):
- **Speed on Adreno**: render passes instead of dynamic rendering, 1024 framebuffers kept (Galar #205: Sword's replay 27.5 → 17.3 ms on the S25+);
  target changes keep the open pass (`DM_VK_PASS_DEFER`); relaxed barriers before draw passes (`DM_VK_BARRIER=full|relaxed`); deferred uniform updates.
  Mali and the rest keep the safe paths. Background pipeline builds (`DM_VK_ASYNC_PSO`) were turned **off** again on 11-13 (graphics glitches).
  Skipped attachment stores are off by default (grey boxes over battle effects).
- **Mali / PowerVR / old phones**: multiply by zero as on the Switch (FMZ, on for PanVK), the NaN rules of min / max / saturate, Maxwell's edge cases for
  log2 / exp2 / rsqrt / sqrt / F2I, dual-source blends in two steps, swapchain fallbacks, a software present when no swapchain can be made, NVN thunks
  without LSE atomics (Snapdragon 680 / 685), Android 12 start fix, PowerVR pipeline-cache and MapPhysicalMemory fixes, a fence per submission when
  timeline semaphores fail. Arceus: transform feedback captures drawn as points for drivers without vertex stores (invisible characters on Mali),
  a start-up vertex canary for drivers that get per-draw vertex sizes wrong (exploding polygons, Arceus #34 / #46).
- **Picture**: FSR 1 upscaling, Colour (Natural / Vibrant / Cinematic), widescreen for both Sword / Shield and Arceus, Lossless Scaling frame generation
  (the player imports their own `Lossless.dll`; **locked off for now** since 11-12, together with Faster rendering on Galar).
- **Frame rate**: frame pacing by frame interval (`DM_VK_PACE_LEAD`); Sword / Shield 60 fps uses the 60 FPS mod's step only, waits for a field area,
  and **the stadium crowd no longer flickers** (was problem 2); Arceus 60 fps at the right game speed. The game's real-time step capped at 50 ms (#232).
  v0.94 "Balance": Auto starts at 85 % render scale and settles, heat stops at 720p, pipelines wait up to 8 ms a frame with 3 workers.
- **Setup**: the fast setup was reverted on 11-12 (#171 / #174 / #175); shader-translation updates are done in place without asking for the game files;
  a cooler mode (half the workers), memory-capped workers on 4 GB phones, the pack written beside the old one and swapped in.
- **App**: save folder in `Documents/<app name>/<title id>/save` for sync tools, camera and stick sensitivity, floating sticks, gyro aiming (Arceus),
  layout editor fixes, the console's shared fonts when supplied, local play over Wi-Fi only, a bug report form, device lost counted as a crash.
- Arceus only: 1440p tried and turned off (the game crashed at start); Far draw distance removed; style move effects off by default (grey boxes).

## Open pull requests (not in any release)

Galar (`origin`), with the twin Arceus PR in brackets:
- #318 `debug-bundle`: debug report ZIP, Android tombstones after our crash record, GPU breadcrumbs, **the unified driver layer** `vk_quirks.h`
  (every per-GPU workaround in one table; checked against the old checks over 1.49 M GPU descriptions). Phone test "in progress" per the PR. [#77 `arceus-debug-layer`]
- #314 `dynamax-heap-id`: crash record names the heap behind the Dynamax crash (#211). See problem 2.
- #313 `galar-lock-futex`: late wakes, opt-in `DM_LATE_WAKE=1`. [#76 `arceus-late-wake`, in `release-045`]
- #312 `galar-vulkan10`: Vulkan 1.0 support. [#73 `arceus-vulkan10`, newest branch: 11-17]
- #309 `galar-vulkan11-notice`: setup screen warns Vulkan 1.0 phones. [#71, in `release-045`]
- #308 `b10-swap-audit`: R11G11B10F → RGBA16F stand-in test (#282, Mali).
- #307 `galar-lse-swap-emu`: LSE swaps on ARMv8.0 done on SIGILL. [#68, in `release-045`]
- #306 `galar-pipeline-libraries`: `VK_EXT_graphics_pipeline_library`, opt-in `DM_VK_GPL=1`. [#65, in `release-045`]
- #302 `cheats`: Atmosphère-format cheat codes in the menu.
- #271 `setup-less-memory`: the shader scan and packs no longer held in memory.

Arceus only: #75 `shader-override` (+ pass times), #70 `arceus-adreno-rp`, #69 `shared-fonts`, #67 `perf-report`, #66 `gyro-throw` (all in `release-045`).

Branches with no PR yet: `capture-button` (Arceus, a camera button that saves the frame to the gallery, #56 / #64), `arceus-1440-pool` (1440p behind
`DM_ARC_RES_FORCE`: the render-target pool 250 → 448 MB at `main+0x277d7f0`), `mali-vertex-formats` (Galar, logs vertex formats the driver can't read;
`DM_VK_VFMT_DENY`), `prewarm-before-play` (ship a pipeline list so the first play is built ahead), `pass-census` (WIP, worktree `Galar-Native-main`).

Paldea: #1 `paldea-sync-081` (v0.82).

## Open problems

The details behind the roadmap, ranked by what the issue trackers say now. Galar has 138 open issues, Arceus 60, Paldea 0.

### 1. Heat and frame drops after the speed-ups

The newest reports on both trackers: "too hot in game" (Galar #332, #333, Arceus #85), drops after updating v0.81 → v0.92 on a Galaxy A55
(Galar #300, Arceus #61), "Optimization worsened" (Arceus #72), slow motion 30 → 20 (Galar #299), snowy areas (Galar #322), Xclipse 530 (Arceus #78).
v0.94's Balance change (Auto starts at 85 %, heat stops at 720p) was the answer to the first of these and has not been checked against them.
- First step: ask a reporter for a **Detailed performance log** (`perf-report`, in Arceus `release-045`; not yet on Galar) or a debug report (`debug-bundle`).
- Compare v0.81 and v0.94 on the same phone, unplugged and cool, `DM_VK_PROF=1`: the changes in between are listed above.

### 2. Sword / Shield: the Dynamax crash

Reported over and over (Galar #211, #298 on Mali-G610, #325, #330; also #331 "snowy arena"). Low / Lowest were kept in TV mode on 11-12 because of a
Dynamax crash in handheld mode; it still happens. `dynamax-heap-id` (PR #314) adds the vtable of each object a crash register points at, to name the heap.
Next: get that build to a reporter, or reproduce on the S25+ (`DM_TEST_CRASH` in `debug-bundle` confirms the crash path works).

### 3. Mali: white screen and invisible characters

Still reported on Galar (#311 Mali-G57, #316 Mali-G720, #301). Many fixes landed (see above); the leads now are the R11G11B10F float target
(`b10-swap-audit`, `DM_VK_B10_AS_RGBA16`) and unsupported vertex formats (`mali-vertex-formats`). Arceus: invisible characters (#47 / #51 / #54 / #57 / #58)
got the point route for transform feedback in v0.43; check whether those reporters confirm it. Low fps on Mali (Arceus #83). **No Mali phone has
ever been tested by us**: every Mali fix was built from reports and checked on Adreno.

### 4. Arceus: purple shadows

Last state (11-09): the lit HDR target gains the purple in `ha_light.bnsh` var72 / var77; the leading suspect is a texture no pass writes
(the ambient-occlusion input), to be listed in `tex_unwritten_one` in Arceus's `game.tsv`. The "No reflections" flat-normals workaround is on by default.
No commit since mentions it; whether it still shows is not recorded. Next steps from the old file still apply: `DM_VK_DRAWLOG=<present>`, then
`DM_VK_TEXOVR=73f1192ee209ab42:<binding>=1,fa3842a1714385d7:<binding>=1`.

### 5. Paldea is behind

Merge PR #1 (after a phone test), then sync from Galar `main` again (90 commits). The sync PR's way of working: Galar's side for the engine, app and
language files; Paldea's README; leave out `site/` and `docs/DEVICES.md`; keep Paldea's two own changes (GPU timestamps for the render scale,
transform feedback batching). Paldea-only switches: `DM_SHADOWS=off|low` (env.txt only). Paldea v0.60 and v0.82 were **never run on a phone**.
Violet outdoors has never been measured (no save past the opening).

### 6. Other open items

- Arceus crashes in battles (#80, #79) and the touch camera stopping after the save screen (#82); ghost touches (#74).
- Chinese text stretched in Arceus (#63): `shared-fonts` (PR #69) is the fix, not released.
- Low-end Adreno 610 blue screen in the welcome cutscene (Galar #319); Dimensity 7060 / 7300 questions (Galar #326, Arceus #84).
- Sword's 24-second freeze loading after Rose's speech: not investigated.
- Sword / Shield YEBIS shaders: 46 are in no game file; they are captured and merged on the next Play.
- Stubs `nvnDeviceGetWindowOriginMode` / `nvnDeviceGetDepthMode`.
- Requested, not started: Arceus UI matching Scarlet / Violet's; a fill-the-screen picture for Scarlet / Violet (Fl4sh's ultrawide formula: with
  `r = (w/h) / (16/9)`, HUD width `1920/r`, offset `(1920 - 1920/r)/2`, FOV scale `r`); Legends: Z-A (Galar #328, a request, not planned).

## Things that wasted time

- **`g_rmu` is held for all of `push_stage`.** Adding locks in functions called from there deadlocks the replay (blue or black screen).
- **Charging hides performance** (see step 6). Results taken that way are meaningless.
- **Releasing several times a day** made it hard to tell which change a report is about (#300: "v0.81 to v0.92"). Ask for the exact version and
  bundle related changes into one release.
- **A shader translator change broke text** (v0.83, chain `300f1183ed07`): reverted to `bebeadf4ac3b`, then fixed in `bc2f6b2e5f0d`. Check text on
  screen after any translator change; the chain id changes every player's shader pack.
- **A Python script named `dis.py`** shadows the standard library's `dis` (capstone imports it).
- **Patch scripts with `\n` in Git Bash heredocs** became newlines in C source. Use raw strings or the file tool.
- A frame dump shows one frame. Confirm which screen it is from.
- In-game taps at A's position type into Sword's name keyboard when it is open.
- The phone's `sed` lacks `\|`; use `grep -v -E`.
- `screenrecord` keeps only frames that change (about 17-20 a second): single-frame flashes can be missed.
