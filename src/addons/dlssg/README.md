# RenoDX DLSS-G (generic DLSS Frame Generation)

Adds NVIDIA DLSS Frame Generation (DLSS-G, through NVIDIA Streamline) to D3D12 games that already run DLSS
Super Resolution through NGX but ship no frame generation. Real DLSS-G only: no FSR3/XeFG/Smooth Motion/LSFG.

First target: **Red Dead Redemption 2, Story Mode only**. Status: **G0** (Streamline bootstrap with frame generation
off, plus observation logs). Frame generation is not available yet.

D3D11 games (first target **Nioh 2**) need a D3D11 → D3D12 bridge because Streamline DLSS-G is D3D12/Vulkan
only; see [D3D11 bridge](#d3d11-bridge-nioh-2--phase-0-plan) (plan only, not built yet).

## Architecture

```
renodx-dlssg.addon64 (src/addons/dlssg)
├─ addon.cpp         per-game PROFILES (exe, Streamline identity, online guard data), gate, settings/status UI
├─ ngx_inputs.hpp    hooks the game's DLSS snippet (nvngx_dlss.dll) as it loads; reads NGX create/evaluate params
├─ reflex.hpp        detours the game's NVAPI Reflex entry points (G0: observe; G1: translate to sl.reflex/sl.pcl)
└─ online_guard.hpp  single-player gate (launch switch + peer-to-peer UDP port); latches frame generation off

src/utils/dlss/streamline_inject.hpp   shared with Forza Horizon 2 (FH2's proven injector, moved verbatim)
  slInit (manual hooking, frame-based tagging, NVIDIA signature check), native CreateCommandQueue /
  CreateSwapChain[ForHwnd] vtable redirects through Streamline proxies (below ReShade), frame tokens, constants,
  tagging, DLSS-G/Reflex output mode, per-config Reflex ownership.
src/utils/dlss/nvngx.hpp               existing NGX hooks (ReShade proxy unwrap) + create/evaluate observers
```

Forza Horizon 2 now uses the same shared injector (`src/games/forzahorizon2/streamline.hpp` is a thin adapter:
config, its own motion vector/depth tagging and DLAA). Its behaviour is unchanged: same Streamline preferences and
feature list, same hook order, same constants, synthesized Reflex.

## How DLSS-G gets its inputs

The game's own DLSS SR evaluate is the generic source, hooked in the DLSS snippet (`nvngx_dlss.dll`) as NGX maps it with
`LoadLibraryExW` (before its CreateFeature). `NVSDK_NGX_D3D12_EvaluateFeature` runs on the game's
command list with every DLSS input in its required state (shader resource), and the game already expects NGX to
clobber command list state there, so recording work in the hook needs no state restore for the game.

| DLSS-G input | From NGX |
| --- | --- |
| Depth, motion vectors | `Depth`, `MotionVectors` resources, tagged with `eOnlyValidNow` on the evaluate's command list (Streamline copies them into its own pool, valid until present; never written, never allocated from game heaps) |
| Extents | `DLSS.Render.Subrect.Dimensions` (+ `DLSS.Input.Color.Subrect.Base`), else create `Width/Height` |
| `mvecScale` | `MV.Scale.X/Y` ÷ MV resolution (render extent with `MVLowRes`, else output extent) |
| `jitterOffset` | `Jitter.Offset.X/Y` (pixels, same convention as Streamline's DLSS) |
| `depthInverted`, `motionVectorsJittered` | create flags `DepthInverted`, `MVJittered` |
| `reset` | `Reset` |
| Camera matrices | identity (dense camera+object MVs; proven on FH2) |
| Backbuffer | Streamline's proxied swapchain; extent tag only |

Games that already use Streamline SR instead (`slSetTag`) need a different adapter (hook `sl.interposer`, as
`utils/dlss/streamline_v2.hpp` does); out of scope until such a game is targeted.

**HUD/UI:** the generic default tags no UI (DLSS-G interpolates the HUD with the scene; static HUD over moving scenes
can smear). Per game, tag a HUD-less colour copy taken right before the first UI draw (owned resource), which
Streamline's guide lists as a significant quality gain on its own. UI colour + alpha is optional and only from an
owned premultiplied target that re-issues the UI draws, never from backbuffer alpha (FH2 lesson).

**Menus/pauses:** frame generation is on only for frames that ran a DLSS SR evaluate (no evaluate: loading, videos,
some menus). Per game: menu shader hashes as an extra veto; with a UI colour + alpha tag, Streamline's
`eEnableFullscreenMenuDetection`; `eRetainResourcesWhenOff` for frequent toggles.

**Reflex:** DLSS-G cannot use a native (non-Streamline) Reflex integration. Games with their own NVAPI Reflex keep
it untouched in G0 (`synthesize_reflex = false`: the injector never calls Reflex or PCL). In G1 the game's
`NvAPI_D3D_SetSleepMode` / `Sleep` / `SetLatencyMarker` calls are translated into `slReflexSetOptions` /
`slReflexSleep` / `slPCLSetMarker` (marker types map 1:1, frame ids become Streamline frame tokens) and are no
longer forwarded to NVAPI: one Reflex, paced by the game's own markers. Calls from Streamline's own modules are told
apart by return address. Games without Reflex use the FH2 path (synthesized markers around present).

## Red Dead Redemption 2

- `RDR2.exe`, D3D12 (`kSettingAPI_DX12`), DLSS through NGX with `nvngx_dlss.dll` in the game folder, NVAPI Reflex
  (`kSettingReflex_On`). Streamline gets its own project id; RDR2's `nvngx_dlss.dll` is not touched.
- **Single player only.** Red Dead Online runs from the same executable and has anti-cheat. The add-on stays inactive
  when launched with an online switch (`-StraightIntoFreemode`, unconfirmed) and latches frame generation off once
  the game binds Rockstar's peer-to-peer port locally (UDP 6672, unconfirmed). G0 logs the local UDP ports the game
  binds in Story Mode; the Online side is captured from outside the process (`Get-NetUDPEndpoint`) on a clean
  install, never with the mod loaded. The guard cannot unload ReShade or Streamline: **never play Online with the
  mod installed** (remove `dxgi.dll` or use the toolkit's undo).

### Mapping plan (G2, needs the game installed)

1. DevKit snapshot in Story Mode (parked camera, HUD visible): find the DLSS evaluate (compute dispatches around NGX),
   the post chain after it (tonemap, bloom, DoF, sharpen), the final scene write into the backbuffer and the first
   UI (Scaleform) draw; note the HUD-off case (photo mode / HUD disabled) for comparison.
2. HUD-less colour: copy the backbuffer-sized target right before the first UI draw into an owned texture
   (copy or re-issued draw, never writes to game targets); tag `kBufferTypeHUDLessColor` with the backbuffer extent.
3. Menus: map/pause/inventory full-screen shaders as veto hashes; confirm whether DLSS evaluates continue under them.
4. Optional: UI colour + alpha from re-issued UI draws into an owned premultiplied target.

## Gates

| Gate | Build | Pass criteria |
| --- | --- | --- |
| G0 | Streamline bootstrap, frame generation off; logs NGX inputs, game Reflex, UDP ports | Game runs normally (also with the D3D12 debug-layer variant); `streamline: initialized`, swapchain/queues created through proxy; DLSS SR evaluates logged with sane extents/flags; game Reflex mode and markers logged; Story Mode local UDP ports vs a clean-install Online capture |
| G1 | Tag NGX depth/MVs + constants; translate game Reflex; frame generation on | Parked camera: smooth 2x with no corruption/hitches, Reflex single-owner (no NVAPI calls forwarded), then motion |
| G2 | HUD-less tag + menu veto (+ optional UI alpha) | HUD stable while panning; menus never interpolated |
| Ship | defaults, docs, pacing capture (PresentMon) | |

Known constraints carried from FH2: RTSS must not hook the game (profile with detection level None); ReShade
screenshots are taken before Streamline, so judge frame generation from video; only NVIDIA-signed Streamline/NGX
DLLs from NVIDIA's releases, Authenticode-checked before deploy.

## Install (G0)

Place `renodx-dlssg.addon64` next to `RDR2.exe` with ReShade (add-on build) as `dxgi.dll`, and NVIDIA's signed
Streamline runtime (`sl.interposer`, `sl.common`, `sl.dlss_g`, `sl.reflex`, `sl.pcl`, `nvngx_dlssg`) in
`renodx-streamline\`. Without that folder the add-on only observes.

## D3D11 bridge (Nioh 2) — phase-0 plan

Status: **plan only**. Nothing is built or deployed for D3D11 yet; the D3D12 path above and Forza Horizon 2 are not
touched by it.

Streamline DLSS-G only runs on D3D12/Vulkan, so a D3D11 game needs a private D3D12 device that owns the real
(Streamline) swapchain, with the game's frames and DLSS inputs moved across on the GPU every frame.

### Target facts (Nioh 2 – The Complete Edition, Steam 1325200)

Read-only look at `D:\SteamLibrary\steamapps\common\Nioh2` and its `ReShade.log`:

- `nioh2.exe` calls `D3D11CreateDevice` (feature level 11_0, flags 0) and later
  `IDXGIFactory2::CreateSwapChainForHwnd`: B8G8R8A8, 3 buffers, flip-discard, windowed (borderless), window class
  `NIOH`. The device is created twice (a probe device first, add-ons are unloaded and reloaded in between), so the
  bridge initialises on swapchain creation, not on device creation.
- `renodx-nioh2` upgrades that swapchain to **R10G10B10A2 + `ALLOW_TEARING`**, HDR10 (ST 2084) colour space, and
  upgrades the scene R11G11B10 targets to RGBA16F. The D3D12 swapchain must match the upgraded description.
- DLSS SR through NGX D3D11 (`nvngx_dlss.dll` 2.1.53 next to the exe; the exe references `NVSDK_NGX_*`). No
  `nvapi64`/Reflex or Streamline strings in the exe: **no native Reflex**, so markers are synthesized (FH2 path).
- ReShade **6.5.1 (add-on API 17)**. This repository builds add-on API 18 (ReShade 6.8.0, the FH2 toolkit build), so
  `renodx-dlssg` would fail to load here exactly like `renodx-dlss5.addon64` does now: its log line is *"requested API
  version (18) is not supported (17)"*, i.e. error 1114 is an API mismatch, not a corrupt file. Testing needs ReShade
  6.8.0 in that folder (API 17 add-ons such as `renodx-nioh2` keep loading on it).
- `renodx-dlss5.addon64` is from the unverified "DLSS 5" add-on family. Upgrading ReShade would make it load and hook
  NGX evaluate next to ours, so it must be **disabled (renamed to `renodx-dlss5.addon64.off`) before any test**; it is
  left in place, untouched, until the user approves the test.
- Zero-byte `d3d11.dll`, `d3dcompiler_46.dll` and `d3dx.ini` (3DMigoto leftovers). ReShade logs `system32\d3d11.dll`
  being used, so they are inert today; left alone, noted as a risk if load order ever changes.

### Prior art (design study only; no code copied)

- **OptiScaler 0.10 "Dx11 with Dx12"** (GPL-3, [`OptiScaler/with_dx12`](https://github.com/optiscaler/OptiScaler/tree/master/OptiScaler/with_dx12),
  created in [`hooks/DxgiFactory_Hooks.cpp`](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/hooks/DxgiFactory_Hooks.cpp)):
  on `CreateSwapChain*` for a D3D11 device it creates the **real D3D11 swapchain on a hidden window**, a D3D12
  device + queue on the same adapter, and the FG (or plain D3D12) swapchain on the **game's HWND**, then returns a
  wrapper (`Dx11wDx12SC`). `GetBuffer` returns the hidden D3D11 swapchain's buffers. On `Present`: copy the D3D11
  back buffer into an owned `SHARED | SHARED_NTHANDLE` texture, `ID3D11DeviceContext4::Signal` a shared
  `ID3D11Fence` that D3D12 opened, `ID3D12CommandQueue::Wait`, copy into the FG swapchain buffer, present the
  hidden D3D11 swapchain with sync 0, then the D3D12 swapchain. Resize/fullscreen/colour space are forwarded to
  both under a present/resize lock. Upscaler inputs come from the NGX D3D11 parameters, copied into rings of shared
  textures (one set per frame in flight, because FG reads them late); R24G8/R32G8X24 depth goes through a compute
  depth-transfer to a shareable format first. Under Wine it waits on the CPU because a GPU D3D11-waits-on-D3D12
  cycle can deadlock when both map to one queue.
- **ShortFuse's RenoDX DLSS add-on** ([`src/addons/dlssfix`](https://github.com/clshortfuse/renodx/tree/main/src/addons/dlssfix),
  [`src/utils/dlss/nvngx.hpp`](https://github.com/clshortfuse/renodx/blob/main/src/utils/dlss/nvngx.hpp)): its D3D11
  support is hooking `NVSDK_NGX_D3D11_Init/CreateFeature/EvaluateFeature` in the game's snippet (preset/DLAA
  overrides). No frame generation and no D3D12 bridge. Those hooks already exist in this repository and are the D3D11
  input source here (D3D11 observers mirroring the D3D12 ones).
- **DLSS5-Feeder** ([jlrouzies-fr/DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)): an NGX DLAA feeder
  (no frame generation). Its D3D11 path creates colour/depth/MV textures as `SHARED_NTHANDLE` on the game's device
  plus shared fences, and runs NGX D3D12 on a device on the same adapter (LUID). Its notes confirm in practice:
  NT-handle sharing needs D3D11.1+, keyed mutexes are not usable on current drivers (fences are), and every copy
  stays GPU-to-GPU. It belongs to the unverified "DLSS 5" ecosystem: design reference only, none of its binaries.

### Chosen design

```
game (D3D11)                                        renodx-dlssg (owned)
 NGX D3D11 Evaluate ──hook──► after real evaluate: CopyResource depth/MV → owned shared ring[slot] (same context)
 ReShade wrapper ─► Present ─► proxy IDXGISwapChain4 (below ReShade) ─► Signal(shared fence) on game immediate ctx
                                  │  back buffers = owned D3D11 SHARED_NTHANDLE textures (game + ReShade + RenoDX
                                  │  render into them)
                                  ▼
                     private D3D12 device (same LUID) ─ queue.Wait(fence) ─ copy back buffer → SL swapchain buffer
                     ─ tag depth/MV/extent (frame-based, ring slot) ─ PCL markers ─ SL proxy swapchain Present
                     ─ Signal(fence) ─ game ctx Wait before that back buffer / ring slot is reused
```

**1. Inputs.** Add `on_d3d11_create_feature` / `on_d3d11_evaluate_feature` observers to `utils/dlss/nvngx.hpp`
(same pattern as D3D12; the NGX parameter parsing in `ngx_inputs.hpp` is API-independent and is reused as is). After
the real evaluate, `CopyResource` depth and motion vectors on the evaluate's context into an owned ring of
`SHARED_NTHANDLE` textures (3 slots, one per frame in flight), opened once on the D3D12 device. Copies touch no
pipeline state. If depth is a typeless depth-stencil format (`R24G8`/`R32G8X24`) that cannot be a shared copy target,
convert it with a small owned shader recorded on an owned deferred context and executed with
`ExecuteCommandList(..., TRUE)` so the game's state is restored. Colour is not needed by DLSS-G (it uses the
back buffer). Jitter, MV scale, extents, depth-inverted, MV-jittered and reset come from the parameters as on D3D12.

**2. Bridge.** Private D3D12 device on the D3D11 adapter's LUID; one shared `ID3D11Fence` (D3D11 creates,
D3D12 opens) for D3D11→D3D12 and back; GPU waits only (CPU wait fallback if a deadlock is ever observed).
Chosen over the alternatives:
- *Share the game's own resources:* impossible, they were not created shareable.
- *OptiScaler's two copies per frame (D3D11 back buffer → shared texture → D3D12 buffer):* the proxy's back
  buffers are themselves the shared textures, so one copy (shared → swapchain buffer) is enough. D3D12 swapchain
  buffers cannot be shared, so that copy is the floor.
- Expected cost at 4K output / 1440p render: back buffer 4K RGB10A2 (~33 MB) + depth and MVs at render size
  (~15 MB each) ≈ 0.1–0.2 ms of copies, plus one cross-API fence per frame. G0 measures it with D3D11/D3D12
  timestamp queries and logs the averages; PresentMon compares latency against vanilla.

**3. Swapchain.** The game gets a proxy `IDXGISwapChain4` whose `GetBuffer` returns owned D3D11 textures in the
upgraded format (shared, render target + shader resource), with `GetCurrentBackBufferIndex` rotating through them.
The real swapchain is D3D12 flip-discard on the **game's HWND**, created through Streamline's proxy factory on the
private queue, same size/format/flags (`ALLOW_TEARING` if RenoDX set it) and buffer count. Forwarded to it:
`ResizeBuffers[1]` (drain the queue, recreate the shared ring), `SetColorSpace1`, `SetHDRMetaData`,
`SetFullscreenState` (borderless is the supported mode; exclusive fullscreen is passed through, as with any flip
swapchain), `GetFrameLatencyWaitableObject`/`SetMaximumFrameLatency`, frame statistics, containing output.
`GetDevice` returns the game's D3D11 device. `DXGI_PRESENT_TEST` is answered without presenting.

**4. Coexistence with ReShade and RenoDX.** The proxy sits **below ReShade**, so ReShade wraps the proxy exactly as
it wraps the game's swapchain today: effects, overlay, input (bound to the game's HWND) and `renodx-nioh2`'s swapchain
upgrade, resource upgrades and final swapchain pass all happen on the D3D11 side, and the bridge copies the finished
frame. Mechanism: ReShade hooks `IDXGIFactory*::CreateSwapChain*` by **vtable replacement** and calls the previous
entry (the real `dxgi.dll` implementation) as its original. Its factory hooks are installed on the game's first
`CreateDXGIFactory*`, before add-ons load, so the add-on reads the pristine vtable slot from `dxgi.dll`'s on-disk image
(same RVA, relocated) and detours that implementation. Order: game → ReShade (RenoDX edits the description) →
bridge (sees the upgraded description, builds the proxy) → ReShade wraps the proxy. The detour only acts for the game's D3D11 device on its window; every other call (including Streamline creating the D3D12 swapchain) passes straight through. If the detour cannot be
placed, the add-on stays off for D3D11 (it does not fall back to an untested path). OptiScaler's arrangement (above
ReShade, real D3D11 swapchain on a hidden window) was rejected: ReShade's overlay input, RenoDX's window-size checks
and `DXGI_MWA_NO_WINDOW_CHANGES` would all bind to the hidden window.

The private D3D12 objects must stay invisible to ReShade, or it would create a second effect runtime and RenoDX would
upgrade a second swapchain. ReShade skips swapchains whose device is not its proxy ("created without a proxy
Direct3D device"), so the D3D12 device is used through its native pointer (ReShade's `device::get_native()` on the
`init_device` event of our own creation call if ReShade wraps it) and the swapchain is created on a native queue. G0
checks `ReShade.log` for exactly one effect runtime.

**5. Reflex.** Nioh 2 has none: the injector's synthesized Reflex/PCL markers (FH2 path) run around the proxy
present, and `slReflexSleep` runs on the game thread when Present returns. One Reflex owner, no NVAPI involved.

**6. Shared code.** `src/utils/dlss/streamline_inject.hpp` gains an entry point for an owned D3D12 device/queue
(Streamline init and options, swapchain creation through the proxy factory, tagging, Reflex), with no hooks on game
D3D12 objects; FH2 keeps its current path unchanged. The bridge lives in a new shared header
(`src/utils/dlss/d3d11_bridge.hpp`) so any D3D11 DLSS game can use it; `addon.cpp` gets a Nioh 2 profile
(`nioh2.exe`, D3D11, no online guard: Nioh 2 has optional co-op but no anti-cheat; test offline/solo anyway).

**HUD/menus:** as on D3D12. G1 tags no UI (HUD interpolated with the scene). G2 maps Nioh 2's first UI draw with the
DevKit and tags an owned HUD-less copy. Frames without a DLSS evaluate (menus, loading, cutscenes if rendered
without DLSS) veto frame generation.

### Risks

| Risk | Mitigation / how G0 shows it |
| --- | --- |
| Present path changes for every D3D11 frame (proxy + copy + fence) | G0a ships the bridge without Streamline first; any regression shows before DLSS-G is involved |
| Pristine-slot detour (ReShade version or other overlays also hooking the factory) | Verify `dxgi.dll` slot target, log the call order; refuse to activate if anything is off |
| Steam overlay / GeForce Experience (`GfeSDK.dll`), Ansel (`AnselSDK64.dll`) hook the D3D11 Present that no longer reaches DXGI | Steam overlay should attach to the D3D12 swapchain; checked in G0a |
| RTSS hooks an injected Streamline swapchain and crashes (FH2) | RTSS profile for `nioh2.exe` with detection None |
| Exclusive fullscreen / mode changes | Borderless is the supported mode; resize + alt-tab are G0a test steps |
| Depth format not copyable to a shared texture | Owned conversion pass on a deferred context (state restored) |
| GPU fence cycle between D3D11 and D3D12 | Only one D3D11→D3D12 wait per frame plus a slot-reuse wait; CPU-wait fallback switch |
| ReShade screenshots are taken before Streamline | Judge frame generation from video |
| Frame cap: DLSS-G needs a reasonable base frame rate | Check Nioh 2's frame-rate setting (unlocked/120) in G0 |
| ReShade 6.5.1 → 6.8.0 upgrade in the Nioh 2 folder | Back up the current `dxgi.dll`; the toolkit's undo restores it |

### Gates (D3D11)

| Gate | Build | Pass criteria |
| --- | --- | --- |
| G0a | Bridge only: proxy swapchain + plain D3D12 swapchain, no Streamline, no frame generation | Image identical to vanilla (SDR and RenoDX HDR10), ReShade overlay and input work, resize/alt-tab/borderless OK, one effect runtime in `ReShade.log`, copy cost and PresentMon latency within ~0.3 ms of vanilla; D3D11 + D3D12 debug-layer variant clean |
| G0b | Streamline on the private device, DLSS-G off, synthesized Reflex; D3D11 NGX inputs copied and tagged | `streamline: initialized`, swapchain through the proxy factory, DLSS evaluates logged with sane extents/flags, no stalls |
| G1 | Frame generation on (2x, then 3x/4x), menu veto | Parked camera: smooth, no corruption; then motion and combat |
| G2 | HUD-less tag from the DevKit mapping | HUD stable while panning; menus never interpolated |
| Ship | Defaults, docs, PresentMon pacing capture | |

### What the user tests (only after approving the deploy)

1. Rename `renodx-dlss5.addon64` → `renodx-dlss5.addon64.off`; back up `dxgi.dll` and replace it with ReShade 6.8.0
   (add-on build, same as FH2). Confirm the game and `renodx-nioh2` HDR still look as before (no DLSS-G add-on yet).
2. G0a: add `renodx-dlssg.G0a.addon64`; play a few minutes in a mission (DLSS on) and in menus; alt-tab, change
   resolution, toggle HDR; check the ReShade overlay (Home). Send `ReShade.log` and the add-on log.
3. G0a-debuglayer variant: same, short run; send the debug output.
4. G0b: same steps with `renodx-streamline\` (NVIDIA-signed DLLs) added; send logs.
5. G1 and later: as on FH2 (parked camera, then motion), phone video for artifacts, PresentMon capture.

Undo at any point: remove the add-on and `renodx-streamline\`, restore the backed-up `dxgi.dll`, rename
`renodx-dlss5.addon64.off` back if wanted.