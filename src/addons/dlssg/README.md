# RenoDX DLSS-G (generic DLSS Frame Generation)

Adds NVIDIA DLSS Frame Generation (DLSS-G, through NVIDIA Streamline) to D3D12 games that already run DLSS
Super Resolution through NGX but ship no frame generation. Real DLSS-G only: no FSR3/XeFG/Smooth Motion/LSFG.

First target: **Red Dead Redemption 2, Story Mode only**. Status: **G0** (Streamline bootstrap with frame generation
off, plus observation logs). Frame generation is not available yet.

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
