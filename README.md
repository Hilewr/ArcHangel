# ArcHangel (HLTas — Stage 2)

**ArcHangel** is an advanced internal TAS (Tool-Assisted Speedrun) program and overlay tailored for **Half-Life 1** (GoldSrc engine running on OpenGL). 

By hooking deep into the game's modules (`hw.dll`, `client.dll`), it introduces precise engine-level time manipulation, movement automation, and an advanced graphical record/replay workbench directly inside the game world.

> ⚠️ **CRITICAL WARNING:** This tool is strictly intended for **Single-Player** mode. Do not attempt to join secure online multiplayer servers with this binary injected. Start the game with the `-insecure` launch option.

---

## ✨ Core Features

### 🕒 Time & Engine Control
* **Engine-Side Slow Motion:** Instead of using slow console commands, the tool directly hooks Windows time sources (`QueryPerformanceCounter`, `timeGetTime`, `GetTickCount`) called by `hw.dll`. Physics frames retain their integrity while execution slows down natively.
* **Smooth Slow-Mo Mode:** Dynamically scales up the game's FPS limit (`fps_max`) when slowed down, ensuring buttery-smooth rendering metrics without violating baseline physical accuracy.
* **Frame-Stepping:** Allows full state pauses and single-frame advancements via engine commands (`pause`).

### 🏃 Automated Movement Mechanics
* **Auto-Bunnyhop:** Holds the jump frame perfectly upon landing and automatically releases it in mid-air.
* **Adaptive Auto-Strafer:** Monitors mouse delta and vectors player acceleration mathematically perpendicular to current velocities, fully adjusting to user view directions.
* **Strafe Helper:** Assist mode choosing optimal `A`/`D` steering inputs depending on real-time mouse physics.
* **Ground Strafe (Duck Spam):** Spams crouching inputs every alternate frame while on the ground for velocity retention tricks.

### 📊 Workbench Recorder & Map Integration
* **Cross-Game Run Recording:** Captures precise 48-byte structural frame data packets (`pos`, `vel`, `yaw`, `pitch`, `buttons`, `msec`, `flags`). Compatible with Half-Life 1 and Counter-Strike 1.6 due to the shared GoldSrc foundation.
* **Frame-by-Frame Input Replay:** Repeats recorded movement patterns flawlessly. Features server-side edict validation and auto-teleportation to the starting line.
* **3D In-World Path Rendering:** Draws dynamic, velocity-colored heat trails (Blue ➡️ Green ➡️ Yellow ➡️ Red) right inside the game world with wall-occlusion toggles.
* **Built-in BSP30 Map Parser:** Reads `.bsp` map geometries directly from the game files, enabling a full 3D/2D orbit map viewer inside the UI with ceiling-clipping options.
* **Jump Statistics:** Tracks last-jump dynamics including pre-strafe velocity, max velocity, peak air-height, and precise units traveled.

---

## 🎨 ImGui Neon HUD & Menu System

ArcHangel injects a fully customizable, responsive **ImGui Graphical Interface** rendered directly through an OpenGL hook via `wglSwapBuffers` / `SwapBuffers`.
* Includes a digital, neon-themed heads-up display showing current 2D speed metrics, ground/air states, and active automation tags.
* Supports **Custom Bindings** mapped through native `WndProc` interceptions, automatically saved to `hltas.ini` placed right next to your `hl.exe`.
* Offers two master aesthetic visual modes out of the box:
  * `0` — **Neon Palette** (Pitch darkness background combined with glowing Cyan & Magenta accents).
  * `1` — **Mono Palette** (Graphite gray workspace with clean white accents).

---

## 🛠️ Low-Level Technical Details (Architecture)

The architecture is built upon automated address translation and internal structural hooks using the **MinHook** library:
* **`HUD_PlayerMove` Hook:** Intercepts real-time physical telemetry packets (`player_index`, `origin`, `velocity`, `flags`) copied inside the client state machine.
* **`CL_CreateMove` Hook:** Appends physics automation calculations (`UserCmd` manipulation) seamlessly before input arrays get shipped over to the host core.
* **`HUD_DrawTransparentTriangles` Hook:** Snaps directly into native 3D matrices prepared by the GoldSrc renderer to blend structural vector arrays into the world space.
* **Signature-less Pointer Scanning:** Scans operational tables inside `client.dll` and `hl.dll` to acquire vital server engine function markers (`pfnClientCmd`, `FindEntityByString`, `SetOrigin`) dynamically.

---

## 📂 Project Structure & Configurations
* `archangel_runs/` — Automatically generated subdirectory containing saved run recordings (`.run` extension using custom binary `AHR2` headers).
* `hltas.ini` — Global configuration cache holding keybindings, operational movement flags, graphics parameters, and theme selections.
* `hltas.log` — Diagnostics file appending operational status checks, window handlers, and initial hook validation values.

---

## 👥 Roadmap / Next Steps
* [ ] Optimize `Strafe Helper` accuracy and consistency across variable frame ticks.
* [ ] Integrate visual weapon cosmetics кастомизация (such as non-WH `Weapon Glow` filters).
* [ ] Fix complex multi-directional vectors for automated side/backwards movement.
