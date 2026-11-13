# Port Ryzom to Vulkan — handoff

_Last updated: 2026-11-11, `vulkan-transforms-perf` merged and verified in game
(branch `vulkan-integrate-verify`, see the last section)_

## Status in one line

**`nel_drv_vulkan` runs the live client on Vulkan: the login/character-selection
screens, the in-game UI, the sky, the landscape, the vegetation and the player
avatar render, validation-clean, with fog, projected shadows on the default
(simple) path and bloom + FXAA on the render-target path.** User vertex programs
(nelvp, glsl330v and the `glsl3vi` insert profile used by GPU skinning/MRM
geomorph) compile and draw. Offscreen render targets exist, so `Shadows = 1`
does not paint the shadow pass into the window, and the shadow receiver's
object-space texgen works on both paths (see the `vulkan-fog-bloom` section).

## Branch

`main/atys-live-n-vulkan` = `main/atys-live` + OpenGL3 renderer (fast-forward
from `origin/atys-n-gl3`, `88e72dc35`) + the Vulkan driver, pushed to `origin`.
The vertex-program work is on local branch `vulkan-nelvp` (nelvp/glsl330v, not
pushed), `vulkan-skinning` (glsl3vi insert programs + GPU skinning, based on
`b7c8b0200`, not pushed), `vulkan-rendertargets` (render targets + shadow maps,
based on `6b20a9341`, not pushed), `vulkan-fog-bloom` (simple-path texgen +
fog, bloom/FXAA, based on `d7334636c`, not pushed) and `vulkan-transforms-perf`
(deferred destroys, batched first-time texture uploads, NLVK_STATS, `glsl3vi`
opt-in; merged into `vulkan-integrate-verify` on top of `4b7aba749` and verified
in game, see the last section).

The GL2 and GL3 drivers are kept. `Driver3D="OpenGL3"` was re-tested after the
Vulkan work: the GL3 mega shaders initialise and the client runs.

## What works (verified)

- `nel/src/3d/driver/vulkan/` implements `IDriver` natively on Vulkan.
- `Driver3D="Vulkan"` (or `"5"`) selects it; `Auto` still picks OpenGL3.
- X11 window + `VK_KHR_xlib_surface`, swapchain, depth buffer (format queried
  from the device), 2 frames in flight, FIFO present, per-image present
  semaphores, render pass kept across swapchain recreation.
- Runtime GLSL→SPIR-V compilation with glslang; pipeline cache keyed by vertex
  layout + render state; shader variants per vertex format (position, normal,
  colour, 4 texcoord sets).
- Textures: RGBA8 conversion, device-local images, staging uploads, in-place
  updates for animated textures, alpha-only textures (fonts) as a white
  coverage mask, deferred destruction at frame boundaries.
- Vertex buffers in host-visible memory through `IVBDrvInfos::lock/unlock`;
  engine index buffers copied into a 16 MB ring per frame.
- TexEnv interpreter (same packing as `CMaterial::CTexEnv`), per-stage UV
  routing (`CVertexBuffer::getUVRouting`, `mapTextureStageToUV`), directional
  lighting from the light table (`setLights`/`setLightTableEntry`).
- Negative-height viewport (GL-like bottom-up), GL depth converted to [0,1] in
  the vertex shader, CCW front face.
- Offscreen render targets: `setRenderTarget`/`getRenderTarget`/
  `getRenderTargetSize`/`copyTargetToTexture`, a shared depth/stencil cache per
  target size, and a LOAD pass to resume the window pass mid-frame. Shadow maps
  write to a real target (`Shadows = 1` is safe). See the render-target section.
- Per-stage tex-coord generation (object-linear, eye-linear, sphere and
  reflection map) and user texture matrices in the simple vertex shader, so
  shadow receivers project the shadow map without `NLVK_MEGA`.
- Fog (linear / exp / exp2) on both the simple and the mega paths, from the
  engine fog state (`enableFog`/`setupFog`/`setupFogMode`), using the same
  eye-space depth and the GL3 black-fog override for additive draws.
- Bloom and FXAA on the render-target path: `supportBloomEffect()` is true,
  `stretchRect()` is a scaled `vkCmdBlitImage`, and user pixel programs
  (FXAA's `glsl300esf`) run on the mega pipeline. See the new section.
- ~60 fps (vsync) in game on RADV, ~600 draw calls/frame.
- Validation layer clean. The only remaining message is gamescope's WSI
  `VkSwapchainPresentModeInfoKHR` warning, which comes from the gamescope
  layer, not from the driver.
- A clean build tree (`build-verify`, from scratch) was built from commit
  `6d39f3d7a` and run: the client reports version `#6d39f3d7a` and the login
  screen renders with all text, validation-clean (evidence:
  `/tmp/opencode/ryzomtest/finalverify.png`).

## Known gaps

1. **Lightmaps / shader types.** The mega shaders implement `CMaterial::LightMap`,
   `Specular` and the TexEnv path, but not PPL and water. Lighting on many
   meshes is still flat; this is the main remaining rendering work. Fog is
   implemented on both paths (see the `vulkan-fog-bloom` section).
2. **Vertex programs.** Implemented on `vulkan-nelvp` and `vulkan-skinning`
   (see the sections below). `glsl3vi` insert programs now compile and draw
   (GPU skinning + MRM geomorph). The avatar still takes the CPU-skinning +
   simple-shader path because the live character has blend shapes, but that
   path now renders it textured; see the shadow-map note.
3. **Render targets.** Implemented (branch `vulkan-rendertargets`):
   `setRenderTarget`/`getRenderTarget`/`getRenderTargetSize`/`copyTargetToTexture`
   and offscreen `ITexture` images. Shadow maps write to a real target, so
   `Shadows = 1` is safe again. Bloom and FXAA are implemented on top of the
   target path (branch `vulkan-fog-bloom`): `supportBloomEffect()`,
   `stretchRect()` (scaled blit) and the user pixel-program path for FXAA.
4. **Shadow projection in the simple path.** Implemented on
   `vulkan-fog-bloom`: the simple vertex shader does object/eye/sphere/
   reflection texgen from the per-draw UBO and applies the user texture
   matrices. The shadow receiver also needed the material zBias to be applied
   to the viewport depth range (`applyDepthRange()`), otherwise its quad lost
   the depth test against the terrain. With `Shadows = 1` and no `NLVK_MEGA`,
   the avatar's shadow is projected on the ground.
5. **Textures are never evicted** from `_TexturesVulkan` (no `ITextureDrvInfos`
   hook): a per-session leak, and a re-used `ITexture` address could show a
   stale texture.
6. **Partial texture uploads.** `uploadTexture` re-uploads the whole texture;
   compressed (DXT) textures are decompressed on upload.
7. **`getZBuffer`/`getBufferPart`** are stubs; `getBuffer` does a swapchain
   readback.
8. **Performance details.** One descriptor set per draw from a per-frame pool,
   no persistent pipeline cache, rings are single-buffered (they grow instead
   of wrapping).
9. Cubemaps are uploaded as 2D textures; `getTotalVideoMemory` is 4 GiB.
10. `copyTargetToTexture()` only supports an offscreen source; copying from the
    window target would have to suspend the swapchain pass.

## Environment / run

    cd ~/Projects/ryzom/ryzom_live_vk2
    cp <repo>/build-vulkan/bin/ryzom_client_dev ryzom_client
    gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 --backend wayland -- \
      env LD_LIBRARY_PATH=<repo>/build-vulkan/lib ./ryzom_client --nopatch 1

**IMPORTANT**: If you don't use gamescope, xdotool and other input tools will
not work, because they'll be running under X under Wayland, which are not
configured to allow virtual inputs.

- `ryzom_live_vk2` is a run dir (data/cfg/examples symlinked to
  `~/Projects/ryzom/ryzom_live`, `client.cfg` with `Driver3D="Vulkan"` at
  1280x800).
- **Screenshots**: host captures are unreliable on this box (KDE can return a
  fully transparent image). Use the driver dump:
  `NLVK_SCREENSHOT=/path/frame.ppm NLVK_SCREENSHOT_FRAME=600` (or
  `NLVK_SCREENSHOT_SEQ=1` for one every 600 frames, `prefix.<frame>.ppm`).
  `convert frame.ppm frame.png` to view.
- Account: after `kill -9` the avatar stays "online" for the network timeout +
  30 s, and repeated login attempts appear to extend it; wait a few minutes
  between runs. Log in with `mats007 passDOSma slot 1 --nopatch 1`, or leave the
  credentials out for the login screen only.
- Driver debug env: `NLVK_VALIDATION=1`, `NLVK_SCREENSHOT[_SEQ]`,
  `NLVK_NOCULL=1`, `NLVK_FORCECLEAR=1`, `NLVK_TESTTRI=1`, `NLVK_NOVP=1` (no
  user vertex programs), `NLVK_NOUBO=1` (force the nelvp/constant path),
  `NLVK_VP_DUMP=<dir>` (GLSL of shaders that fail to compile), `NLVK_MEGA=1`
  (mega path for every draw).
## GL3 mega shaders (`NLVK_MEGA=1`) — merged

The GL3 mega vertex/pixel shaders and the four builtin UBOs are now in the main
branch (`driver_vulkan_mega.cpp`, `driver_vulkan_mega.h`,
`driver_vulkan_mega_render.cpp`), selected with `NLVK_MEGA=1`. The default
simple path is untouched and remains what runs without the variable.

State after rebasing the branch onto the current main and merging it
(merge commit `72aeb9c61`, mega commit `847eb3d74`):

- Login screen with `NLVK_MEGA=1`: renders through the mega path (VP + PP
  compiled), pixel-identical to the simple path except the blinking caret
  (AE 319 px of 1,024,000); validation-clean.
- In game with `NLVK_MEGA=1` (verified 2026-11-10 ~20:15, character `Megana`,
  screenshots `/tmp/opencode/ryzomtest/lm-mega.png` and `lm-simple.png`): the
  ground now matches the simple path pixel for pixel (ground band AE = 0). The
  earlier magenta ground was the fog map's pink fog colour applied by the mega
  shaders plus additive passes adding fog (fixed with the GL3 black-fog
  override). Vegetation is still black (vertex programs).
- Validation-clean for the mega path (the only warning remains gamescope's WSI
  present-mode one).

Known gaps in the mega path (from the subagent that ported it):
- `CMaterial::LightMap` multi-pass staging is ported (see the section below),
  but no LightMap material was in view at the verification spot, so it still
  needs an in-game check somewhere with lightmaps (town/interior).
- Cubemaps on stages other than 1 fall back; cubemap upload is not implemented.
- TexGen and EMBM are not staged (the VP generator supports the modes).
- User vertex programs are implemented (see the `vulkan-nelvp` section below);
  the `glsl3vi` insert profile (GPU skinning) is not.

Next step there: verify a LightMap material in game (town/interior), then
re-enable fog in the mega object UBO once fog is also ported to the simple
shader (it is currently forced off so both paths agree). **Done on branch
`vulkan-lightmap`, see the section below.**

## Mega world staging: LightMap multi-pass (branch `vulkan-lightmap`)

The mega path now draws `CMaterial::LightMap` through the GL3-style
multi-pass staging, ported from `driver_opengl3_material.cpp`
(`computeLightMapInfos`, `beginLightMapMultiPass`, `setupLightMapPass`):

- `beginMaterialMultiPass()` computes the lightmap LUT/pass count, saves the
  base stage textures and returns the pass count (still `1` on the simple
  path, so the simple path is unchanged);
- `setupMaterialPass()` stages stages 0..N-2 with the pass' lightmap textures
  (factor in `constant[stage]`, LMC ambient added to self-illumination) and
  stage N-1 with the material texture, exactly like GL3;
- the per-pass material UBO overrides (white/half diffuse, zero specular,
  `nlLightMapScale`, zero constants/no-lightmap case, identity texture
  matrices), object UBO overrides (LMC self-illumination, zero light factors
  on additive passes) and the black fog on additive blends/passes mirror GL3;
- `renderTriangles`/`renderLines`/raw/quad draws loop over the pass count like
  the GL3 driver; `renderSimpleTriangles` (landscape) stays single-pass.
- `canUseMegaPath()` now accepts `CMaterial::LightMap`.

What actually made the ground magenta under `NLVK_MEGA=1`: **scene fog**, not
the staging. The moor is foggy and the fog map gave a pink
`FogColor` (201,144,133) at the test time; the mega shaders apply it to the
far landscape backdrop, while the simple path has no fog at all and shows the
backdrop's flat green texture. The additive landscape passes additionally
added the fog colour on top (GL3 overrides the fog colour with black for
additive blends; the Vulkan driver now does that too). Since the simple path
has no fog yet, the mega object UBO now keeps `fogEnabled = 0` so both paths
agree; the generated fog code is still there. Re-enable it when the simple
path grows fog (or when the mega path replaces it).

After the fix, at the same spot (`lm-simple.png` / `lm-mega.png` in
`/tmp/opencode/ryzomtest/`) both paths show the same green backdrop
(97,122,44 at the sampled pixel); the mega path adds its lighting/texenv
shading on top. Still untested in game: a `LightMap` material was not in view
at the test spot, so the new lightmap staging needs a check on a
static-lightmapped object. Cubemap/mega and TexGen staging are unchanged.

## Vertex programs (branch `vulkan-nelvp`)

User vertex programs now compile and draw. Implemented in
`nel/src/3d/driver/vulkan/driver_vulkan_vp.cpp` (plus small changes in the
header, the mega resource setup and the draw path):

- `convertNelvpToGLSL()` is a port of the GL3 converter. It parses nelvp with
  `CVPParser`, emits `#version 450` GLSL with the used vertex inputs at their
  Vulkan locations, declares **all 16 generic varyings** the mega pixel program
  may read (Vulkan requires a matching vertex output; GL SSO did not), the
  `NlNelvpConstants` `vec4 c[N]` block at binding 4, and the same instruction
  set as GL3 (MOV/ARL/ADD/MUL/MAD/DP3/DP4/DST/LIT/MIN/MAX/SLT/SGE/RCP/RSQ/
  EXPP/LOG). The epilogue rebuilds `ecPos` from `inverseProjectionBasis *
  gl_Position` (before the z remap) for fog, always synthesizes PPL `worldPos`,
  adds a world-space `normal` when the program reads v2, and remaps
  `gl_Position.z` from the GL clip range to `[0, w]`.
- Engine GLSL vertex programs (profile `glsl330v`: the wind tree, vegetable and
  landscape UBO twins) are accepted as-is: the builtin UBO blocks and the
  blocks described by `CSource::UniformBufferFormats` are injected at bindings
  0-3 and 4-6, missing generic outputs are appended, and `main()` is wrapped to
  remap the clip-depth z the same way. glsl3vi (GPU-skinning snippets) is
  rejected: `supportVertexProgram(glsl3vi)` stays false, so the engine keeps
  its CPU skinning path.
- `compileVertexProgram()`/`activeVertexProgram()` compile to a VkShaderModule
  and create a pipeline per (program, vertex layout, render state); the pipeline
  reuses the mega descriptor set layout and mega pixel stage, and the vertex
  stage is replaced by the user program. `CPipelineKey` gained `UserVP` and
  `MegaSpecular`, so `m_VPSpecularOutput` (GL3 semantics) picks the pixel
  stage variant that adds `specularColor`.
- `setUniform1f..4ui`, the `CVector`/`CRGBAF` overloads, `setUniform4x4f`,
  `setUniform4fv/4iv/4uiv`, `setUniformMatrix` and `setUniformFog` write into
  the active program's `NlNelvpConstants` UBO (same register layout and matrix
  transposes as GL3's `getNelvpUB` path). `bindUniformBuffer()` stores the
  engine UBO for binding 4 (e.g. `NlVegetable`, `NlWindTree`, `NlLandscape`);
  the draw uploads the right block into the per-frame uniform ring. The
  binding is driver state, exactly like GL3: the landscape binds its shared
  block once per frame and calls `activeVertexProgram()` again before every
  draw, so clearing the binding on activation makes the block read zeros.
- `supportBuiltinUBO()` stays true (landscape/wind tree/vegetable prefer their
  GLSL UBO twins). `NLVK_NOUBO=1` forces the nelvp path for A/B testing and is
  how the converter was verified end to end. `NLVK_NOVP=1` restores the old
  behaviour (no user vertex programs at all). `NLVK_VP_DUMP=<dir>` writes the
  GLSL of shaders that fail to compile.
- `initMega()` now always runs (it only allocates the mega descriptor layout,
  pipeline layout and light-table buffers); the mega path is still only used
  for all draws with `NLVK_MEGA=1`, and for user-VP draws otherwise.

Verified in game (character `Megana`, run dir
`~/Projects/ryzom/ryzom_live_vk2_mega`, 1280x800, screenshots in
`/tmp/opencode/ryzomtest/vp/`):

- 24 user programs compile: the wind-tree UBO GLSL program, all 16 nelvp wind
  tree variants, the four landscape UBO programs (including the LightMap twin)
  and the three vegetable unlit GLSL programs (`grep "compiled user vertex"`).
  No "failed to compile" lines.
- UBO path (`g7/f.ppm.8400.png`): the moor now renders a textured green ground
  with dense grass/flowers instead of the flat green backdrop, and the first
  draw log confirms the wind-tree/landscape/vegetable programs are active.
- nelvp path (`NLVK_NOUBO=1`, `noubo/f.ppm.9000.png`): the landscape, lit
  vegetable and wind-tree nelvp variants all draw through
  `NlNelvpConstants`; the ground and vegetation look the same as the UBO path.
- `NLVK_NOVP=1` (`g5/f.ppm.11400.png`) is pixel-identical to the old flat
  green backdrop with no vegetation, confirming the change is isolated.
- Login screen with `NLVK_MEGA=1 NLVK_VALIDATION=1`
  (`login_mega/f.png`): renders correctly; the only validation messages are
  gamescope's `VkSwapchainPresentModeInfoKHR` warnings.

Not fixed / out of scope here: the player avatar is still a black silhouette
(the live client uses CPU skinning, which does not use a vertex program, and the
simple-path material/lighting for that mesh is still wrong); background tree
foliage and the sky are still black/white from the missing sky material and
lighting. The `glsl3vi` insert profile (GPU skinning and MRM geomorph) is the
next step if animated characters matter more than the remaining material work.

Also fixed while testing: the simple vertex shader emitted `(void)routing;`
(not valid GLSL) for vertex formats without texture coordinates, which made
every such pipeline fail and spam the log; the index ring is now 4-byte
aligned (16- and 32-bit draws share it) and vertex buffer deletion always
defers to a frame boundary (destroying one between frames invalidated a frame
still in flight and crashed RADV). The per-draw uniform ring and the index ring
no longer wrap or destroy themselves mid-frame either: dense vegetation reaches
tens of thousands of draws per frame, so both are sized generously up front
(128 MiB and 64 MiB) and any growth allocates a new buffer while the old one
stays alive until the next frame boundary. With those fixes the in-game
validation log after 21:33 contains no driver errors (the only messages are
gamescope's WSI ones, the pre-existing missing `VK_EXT_surface_maintenance1`
extension warning, and benign "vertex attribute at location N not consumed"
warnings for user VP pipelines, which supply the full 16-slot vertex input).
Final screenshot: `/tmp/opencode/ryzomtest/vp/final2/f.ppm.8400.png`.


    cd ~/Projects/ryzom/github/atys-live/ryzomcore
    CXXFLAGS="-D__STDCPP_DEFAULT_NEW_ALIGNMENT__=16" \
    cmake -S . -B build-vulkan -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_BUILD_TYPE=Debug -DFINAL_VERSION=OFF \
      -DWITH_RYZOM_LIVE=OFF -DWITH_RYZOM_CLIENT=ON \
      -DWITH_RYZOM_SERVER=OFF -DWITH_RYZOM_TOOLS=OFF \
      -DWITH_NEL_TESTS=OFF -DWITH_NEL_SAMPLES=OFF -DWITH_QT=OFF -DWITH_QT5=OFF \
      -DWITH_DRIVER_OPENGL=ON -DWITH_DRIVER_OPENGL3=ON -DWITH_DRIVER_VULKAN=ON \
      -DWITH_LUA51=OFF -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
      -DLUA_LIBRARY=/usr/lib/x86_64-linux-gnu/liblua5.3.so \
      -DLUA_LIBRARIES="/usr/lib/x86_64-linux-gnu/liblua5.3.so;/usr/lib/x86_64-linux-gnu/libm.so" \
      -DLUABIND_INCLUDE_DIR=/home/nuno/Projects/ryzom/luabind-install/include \
      -DLUABIND_LIBRARY_RELEASE=/home/nuno/Projects/ryzom/luabind-install/lib/libluabind_lua53.a \
      -DBREAKPAD_INCLUDE_DIR=/home/nuno/Projects/ryzom/breakpad/src \
      -DBREAKPAD_CLIENT_LIB=/home/nuno/Projects/ryzom/breakpad/src/client/linux/libbreakpad_client.a
    ninja -C build-vulkan -j 8 ryzom_client_dev lib/libnel_drv_vulkan.so lib/libnel_drv_opengl3.so

- **Every cmake invocation needs `CXXFLAGS=...`** (the tree rebuilds
  `CMAKE_CXX_FLAGS` from `$ENV{CXXFLAGS}` and breakpad needs the macro).
- **Lua 5.3 + luabind-lua53 are mandatory** for the live 2026 data.
- Vulkan link order: `glslang glslang-default-resource-limits SPIRV
  SPIRV-Tools-opt SPIRV-Tools pthread` (static libs).
- Packages: `libvulkan-dev glslang-dev glslang-tools spirv-tools libshaderc-dev
  vulkan-validationlayers`.

## Next steps (in order of value)

1. **Shadow projection in the simple path** (see gap 4): port the mega vertex
   program's tex-coord generation to the simple shader, or switch the default
   path to the mega shader. Until then shadows only project with
   `NLVK_MEGA=1`, where the mega path also tints the scene (fog parity, next
   item).
2. Fog parity: the mega shaders implement fog but it is currently disabled
   (`stageMegaObjectUBO`) because the simple path has none. Port fog to the
   simple shader (or make the mega path the default) and re-enable
   `data.fogEnabled`; GL3's black-fog overrides for additive blends and
   LightMap passes > 0 are already in place.
3. Bloom / FXAA / water reflection now that render targets exist:
   `supportBloomEffect()` is false, `stretchRect()` returns false and the bloom
   render targets come from `CRenderTargetManager` (`CTextureOffscreen`), which
   exercises the same code path as shadow maps.
4. Verify the LightMap multi-pass staging in game on a static-lightmapped
   object (no LightMap material was in view at the test spot).
5. Texture eviction via an `ITextureDrvInfos` implementation.
6. Partial texture uploads, BC textures, real cubemaps.

## Review findings already fixed

- std140 mismatch in `SSimpleUniforms` (lights were garbage) — fixed with the
  separate `vec4` arrays and a `static_assert` on the block size.
- `clear2D` used to open the frame before storing the colour — fixed (and
  `vkCmdClearAttachments` when the pass is already recording).
- Frames only opened by `clear2D` — now opened lazily by the first draw too.
- Viewport/scissor set before the frame opened were lost — applied when the
  frame opens.
- Render pass (and therefore cached pipelines) destroyed on resize — the render
  pass now lives for the device lifetime.
- `renderRawQuads` ignored `startVertex`; `setCullMode`/`setColorMask` were
  ignored; descriptor allocation failure still drew; source texture format was
  read after conversion (alpha/font branch dead); rings wrapped mid-frame.
- Shutdown now releases pipelines, layouts, pools, shaders, textures, rings and
  the debug messenger; the copied event emitter is renamed
  `CVulkanEventEmitter` to avoid duplicate symbols in static builds.
- `(void)routing;` was emitted by the simple vertex shader for vertex formats
  without texture coordinates; `(void)` is not valid GLSL, so every such
  pipeline failed, spamming the log and skipping those draws.
- The index ring mixed 16- and 32-bit entries without aligning the offsets, so
  every UINT32 draw after a UINT16 draw had a misaligned
  `vkCmdBindIndexBuffer` offset.
- The uniform and index rings wrapped mid-frame, overwriting data referenced by
  draws already recorded in the command buffer, and grew by destroying the
  buffer that the recording command buffer still referenced. Both now grow by
  switching to a larger buffer and deferring the old one to the next frame
  boundary; the uniforms start at 128 MiB and indices at 64 MiB.
- Vertex buffer deletion used to destroy the VkBuffer immediately when no frame
  was open; a previous frame still in flight could then reference a destroyed
  buffer (validation errors, RADV SIGSEGV during zone-in). All deletion now
  goes through the deferred (frame-boundary) path.
- `activeVertexProgram()` must not clear the bound user UBO: the landscape
  binds its shared `NlLandscape` block once per frame and re-activates the
  program before every draw, so clearing it made the shader read zeros (the
  ground rendered white instead of textured).

Still open from the review: texture eviction (gap 4), `getZBuffer`/`getBufferPart`,
`supportBlendConstantColor` returns true without setting blend constants,
`getClipSpaceZMin` returns 0 while the projection is GL-style (affects
`CLandscapePolyDrawer`), depth range only stored (not applied), device features
requested without querying, and the per-frame rings being single-buffered.

## Reference map

- Driver interface: `nel/include/nel/3d/driver.h` (interface version `0x74`).
- Driver selection: `ryzom/client/src/init.cpp:1064`, `nel/src/3d/dru.cpp`
  (`CDRU::createVkDriver`), `ryzom/client/src/client_cfg.cpp`.
- GL3 attribute locations: `driver_opengl3_vertex.cpp` `GLVertexAttribIndex`.

## Vertex programs — merged

`driver_vulkan_vp.cpp` ports `convertNelvpToGLSL`; `NlNelvpConstants` is at
binding 4; `supportVertexProgram`/`compileVertexProgram`/`activeVertexProgram`
work for nelvp and glsl330v. In game, 24 user programs compile (wind tree UBO +
16 nelvp variants, 4 landscape, 3 vegetable) and draw; evidence
`/tmp/opencode/ryzomtest/vp/` (`g7`, `final2` = UBO path, `noubo` = nelvp path,
`g5` = old behaviour with `NLVK_NOVP=1`). Debug env: `NLVK_NOVP=1`,
`NLVK_NOUBO=1`, `NLVK_VP_DUMP=<dir>`.

Not supported before `vulkan-skinning`: glsl3vi insert programs (GPU
skinning/MRM geomorph) were rejected. They are now implemented; see the next
section.

## glsl3vi insert programs + GPU skinning (branch `vulkan-skinning`)

`glsl3vi` is not a complete shader: `nel/src/3d/gpu_skin_vp.cpp` provides a
snippet that defines

```glsl
void nlPreTransform(inout vec4 pos, inout vec3 norm, inout vec4 tc0, inout vec4 tangent)
```

and the engine expects it to be spliced into the mega vertex program. The
Vulkan driver now does that:

- `megavkVPGenerate(result, fogOrPpl, insertSource)` emits the snippet after the
  varying declarations, declares `nlTangent` and calls `nlPreTransform()` on the
  vertex inputs before `modelViewProjection * nlPos`, like GL3.
- The engine snippet uses `gl_VertexID` (the GL spelling); the generator rewrites
  the token to `gl_VertexIndex` for Vulkan.
- The insert program's `UniformBufferFormats` declare `NlMorph` at
  `UBBindingVertexProgram` (binding 4) and `NlSkeleton` at `UBBindingSkeleton`
  (binding 6). `buildVertexProgramUBOHeaders()` (now shared with the raw GLSL
  user programs) prepends those blocks to the generated mega VP.
- `compileInsertVertexProgram()` caches the compiled variants in
  `_InsertVPShaders` keyed by `insertVertexShaderKey(id, fogOrPpl, hwClip)`.
  Only `(1, 0)` is generated today: the mega pixel stage is always the fog/PPL
  variant and `supportVertexProgramClipPlanes()` is false, so no `hwClip`
  variant can be selected. The key keeps both GL3 bits for later.
- `bindMegaDescriptors()` stages binding 4 from `UBBindingVertexProgram` (morph)
  and, for insert programs, binding 6 from `UBBindingSkeleton` (bones). The
  skeleton model binds the bone UBO once per skeleton, before its draws.
- `supportVertexProgram(glsl3vi)` returns true, so `CSkeletonModel` takes the
  GPU path and `CMeshMRMSkinnedGeom::buildGPUSkinVB()` starts building the
  combined skin VB/index buffer.
- `PaletteSkin` (UChar4) is bound as `VK_FORMAT_R8G8B8A8_USCALED`, matching GL3's
  non-normalized integer attribute: the shader reads the raw bone indices 0..255
  in a `vec4`.

Evidence in game (run dir `~/Projects/ryzom/ryzom_live_vk2_mega`, 1280x800):

- `grep "compiled insert vertex program"`: both `GPU Skinning Insert` (MRM
  geomorph) and `GPU Skinning Simple Insert` compile; no shader/UBO errors.
- Character selection (`/tmp/opencode/ryzomtest/final/skin.ppm.2400.ppm`): the
  preview character is entirely drawn through the insert programs (`userVP=1`
  for `vflags=0x31c7`, `userVP=2` for `vflags=0x3007`) and is textured.
- In game every skinned draw logs the insert path; the avatar itself takes the
  CPU path (blend shapes) and now renders textured with `Shadows = 0`
  (`/tmp/opencode/ryzomtest/final/skin.ppm.13200.ppm`; torso samples
  `(214,197,183)` and `(177,166,156)`, not `(0,0,0)`).
- Login/character screens still render. Validation is clean apart from the
  pre-existing gamescope WSI warnings and benign "attribute not consumed"
  warnings: `/tmp/opencode/ryzomtest/final/run.log`.

### The black-silhouette mystery: shadow maps, not skinning

The player avatar is a **CPU-skinned** mesh (the live character has blend
shapes), so it does not use the insert programs in game; the char-selection
preview and NPCs do. The black silhouette was the client's **shadow-map pass**:
`CDriverVulkan::setRenderTarget()` is a stub that returns false and does
nothing, so `CShadowMapManager` keeps rendering shadow casters with its black
depth material straight into the main framebuffer. With `Shadows = 1`
(`client.cfg` default in the run dir) every character/NPC/tree is black.

Workaround used for the verification: `Shadows = 0` in
`~/Projects/ryzom/ryzom_live_vk2_mega/client.cfg` (original saved as
`client.cfg.shadows1.bak`). The real fix is render targets, which are also
needed for bloom/FXAA/water reflection.

### Simple-path fixes found while chasing the avatar

These are driver bugs independent of insert programs, kept because they make the
CPU-skinning path match the mega programs:

- The simple vertex shader used the material colour for lit materials. It now
  uses the material **diffuse** when lighting is on and the material colour when
  unlit, like the mega VP (`NlSimple` gained a `vec4 diffuse`; the block is now
  336 bytes).
- Primary/vertex colours: unlit draws replace the material colour (they used to
  multiply, turning CPU skin VBs with black primary colours into black
  silhouettes); lit draws use them only when
  `IDRV_MAT_LIGHTED_VERTEX_COLOR` is set.
- The simple path now transforms the world-space light direction/position into
  eye space with `_ViewMatrix` (the shader lights eye-space normals); it used
  world-space lights before.
- Cube textures keep the draw on the simple path: `uploadTextureCube()` is a
  stub that creates a 2D view, so the mega `samplerCube` variant would bind a 2D
  view and trip `VUID-vkCmdDrawIndexed-viewType-07752`. This also removes the
  black Specular-environment draws until real cube uploads exist.

## Render targets + shadow maps (branch `vulkan-rendertargets`) — merged

`CDriverVulkan::setRenderTarget()` is implemented, so the shadow-map pass has a
real target (`driver_vulkan_target.cpp`):

- **Offscreen images.** A texture with `ITexture::getRenderTarget()` gets a
  `VkImage` created with `COLOR_ATTACHMENT | SAMPLED | TRANSFER_SRC/DST`
  (`ensureRenderTarget()`, `createTextureImage()`), a `VkFramebuffer` built on
  the offscreen render pass, and a sampled view/sampler the normal draw paths
  already use. Images are recreated when a texture changes size or was created
  before it was flagged as a target (its upload is then dropped). Uploads skip
  render targets entirely (`setupTextureEx()` early-out), otherwise every
  `setRenderTarget()` (which touches the texture) would overwrite the pixels.
- **Depth/stencil cache.** `CDepthTargetVulkan` (_DepthTargets) mirrors GL3's
  `CDepthStencilFBO`: one depth image per (size, format) shared by every target
  of that size; the first pass using it transitions it out of UNDEFINED.
- **Pass switching.** One render pass is recorded at a time. `setRenderTarget()`
  ends the current one and begins the destination. Returning to the window uses
  `_ResumeRenderPass` (same attachments, LOAD ops, so it is compatible with
  `_RenderPass` and shares its pipelines) and the offscreen pass uses
  `_OffscreenRenderPass` (RGBA8 + the window depth format). `swapBuffers()`
  recovers if a target is still bound. `CPipelineKey::Target` selects the pass
  for pipeline creation.
- **Layouts.** A target's colour image is left in `SHADER_READ_ONLY_OPTIMAL`
  when its pass ends, which is the layout every sampled descriptor declares;
  `beginOffscreenPass()` transitions it back. Because the offscreen pass'
  initial/final layouts equal the attachment layout, no implicit transition is
  done by the pass, so those barriers are explicit.
- **Viewport/scissor** are computed from `currentTargetExtent()`, i.e. the bound
  target or the window; a cached scissor is clamped to the new target because
  the aspect ratios differ. `clear2D()`/`clearZBuffer()` clear the target.
- **`copyTargetToTexture`** copies (vkCmdCopyImage, RGBA8 both sides) from the
  bound offscreen target into another texture's level 0, flipping Y for NeL's
  bottom-up rects. Copying from the window target is not implemented.
- `getRenderTarget()` returns the bound texture, `getRenderTargetSize()` its
  size (window size otherwise), same contract as GL3.

Also staged in the mega path (prerequisite for the shadow *projection*):

- `stageMegaMaterialUBO()` now fills `texGenMode[]` from
  `CMaterial::getTexCoordGen()/getTexCoordGenMode()` (the mega VP already had
  the `texMatrix` * `nlPos` code; it was hardwired to disabled).
- `bindMegaDescriptors()` sets `vkCmdSetBlendConstants` (the mega pipelines
  declare that dynamic state; it was only set on the simple path, which made
  validation complain about constant-blend materials).

Verified in game (`~/Projects/ryzom/ryzom_live_vk2_mega`, `Shadows = 1`,
`NLVK_VALIDATION=1`, 1280x800):

- `Vulkan: render target 256x256 created` once, then
  `Vulkan: rendering into offscreen target 256x256`; the per-600-frame log line
  reports target binds (3600 per 1800 frames with shadows on, 0 with `Shadows=0`).
- **No new validation messages** from any of the new paths (the remaining ones
  are pre-existing: the swapchain-maintenance1 extension warning, the
  `VkSwapchainPresentModeInfoKHR` present warning and "vertex attribute not
  consumed" pipeline warnings).
- The avatar stays textured with `Shadows = 1` (skin/cloth colours, no black
  silhouette): the caster pass goes to the offscreen target.
- The projected shadow on the ground is only visible with `NLVK_MEGA=1`, where
  tex-coord generation is implemented: a soft dark patch follows the avatar on
  the moor. With the default (simple) path nothing is projected - the receiver
  material has no vertex UVs and the simple shader ignores `texGenMode`.
- `NLVK_MEGA=1` still tints the whole scene (fog parity), so it is not a usable
  default yet.

## Accounts (one per concurrent client!)

| account | password | slot | notes |
|---|---|---|---|
| `mats007` | `passDOSma` | 1 | main account |
| `mats006` | `passDOSma` | 1 | second account, second concurrent client/agent |
| `mats005` | `passDOSma` | 1 | third account, third concurrent client/agent |

Slot `0` is the first character: those avatars are in a city, which is useful
to test interiors/lightmaps. Slot `1` is the open-field character used so far.

Only one client per account. When several agents need to run the game at the
same time, assign each a different account explicitly in the agent prompt.
Never run two clients with the same account.

## Login procedure (for humans and subagents)

The live account is single-session. A client that is killed still owns the
session for the network timeout + 30 s, and every failed login attempt seems to
restart that timer, so "Your account is already in online state" can persist for
a while. Do it like this:

1. Before starting: `pgrep -a ryzom_client` and `pgrep -a gamescope`. Only kill
   clients you started yourself; never kill the user's session.
2. Log in with:
   `./ryzom_client mats007 passDOSma 4294967295 1 --nopatch 1`
   The positional arguments are `<login> <password> <shard_id> <slot>`: the
   third one is the shard (use `4294967295` for "the current one"), the fourth
   the character slot (1 = Megana). With a slot the client auto-enters the game.
3. To stop: leave the game through `Escape` -> "Quit Ryzom" -> "Quit Now"
   (or `/tmp/opencode/ryzomtest/stop_client.sh` when the window is up), and wait
   for the process to exit. Avoid `kill -9`.
4. If the login says "already in online state": wait 2-3 minutes and try once
   more; do not retry in a loop (each failure extends the wait).
5. Wait at least 30 s between two automated runs, and never run two clients with
   credentials at the same time.
6. Screenshots: `NLVK_SCREENSHOT=<path>.ppm NLVK_SCREENSHOT_FRAME=<n>` (or
   `NLVK_SCREENSHOT_SEQ=1` for one every 600 frames, which only fires at
   multiples of 600 at or after `<n>` - pick `<n>` before the moment you want
   and expect the first file a bit later). Host screenshots are unreliable when
   the session is locked.
7. The run dir used for automated tests is `~/Projects/ryzom/ryzom_live_vk2_mega`
   (`client.cfg.shadows1.bak` is the full config with `Shadows = 1`, which is
   what the shadow-map tests use; the generated minimal `client.cfg` files in
   `/tmp/opencode/rt-run.sh` work too, but keep the whole config when testing
   client options so nothing else changes).

## glsl3vi inserts (GPU skinning) — merged

Insert programs are spliced into the mega VP (`nlPreTransform`, `NlMorph` at
binding 4, `NlSkeleton` at binding 6). `GPU Skinning Insert` and
`GPU Skinning Simple Insert` compile and draw; the character-selection preview is
textured. `compileVertexProgram failed` count is 0 for all 24+ programs.

**The black characters in game are NOT a vertex-program problem**: with
`Shadows = 1`, `CShadowMapManager` drew shadow casters into the main framebuffer
because `CDriverVulkan::setRenderTarget()` was a stub returning false. Render
targets are now implemented on `vulkan-rendertargets` (see that section): the
caster pass goes to an offscreen target and the avatar stays textured. The
shadow *projection* still needs tex-coord generation in the simple path.

Also fixed with this merge: the simple path now uses the material diffuse for lit
draws and the mega VP's vertex-colour rules, and transforms light directions into
eye space (this is what made objects black even without shadow maps). Cube
materials stay on the simple path until cube upload exists.

A full-pink screen seen during loading is the frame clear colour (the moor's fog
colour) with no world geometry drawn yet; it is not a crash.

## Reliable login procedure (use this, it works ~99% of the time)

The account is single-session and a killed client keeps it for the network
timeout + 30 s. Before the real login, force the session to be clean:

    ./ryzom_client mats007 passDOSma          # no slot: errors "already logged in" or waits at char select
    sleep 20                                  # debug client start is slow; 10 s is not always enough
    kill %1                                    # or kill -9 the pid
    sleep 40
    ./ryzom_client mats007 passDOSma slot 1 --nopatch 1

No screenshots/keys needed for the guard step. Then use the game normally
(Escape -> Quit Ryzom -> Quit Now, or the Lua scripting available in the debug
build — `-DWITH_RYZOM_LIVE=OFF` — for a clean logout instead of xdotool).

## Render targets — merged

`driver_vulkan_target.cpp` implements offscreen render targets: COLOR_ATTACHMENT
images/framebuffers, a shared depth/stencil cache, pass switching (a second LOAD
render pass so the main scene survives an offscreen pass), target clears,
`getRenderTargetSize` and `copyTargetToTexture`. `setupTextureEx()` refuses to
upload render targets (so rendering into them is not overwritten).

Evidence (live client, Shadows = 1, NLVK_VALIDATION=1): `render target 256x256
created` / `rendering into offscreen target 256x256` every frame; the avatar's
torso went from 76% pure black to fully textured (rgb(174,160,126), 0% black),
same for skirt and legs; ground unchanged as control; no new validation errors.
Images: `/tmp/opencode/rt/before_2400.png` and `/tmp/opencode/rt/final_2400.png`.

Now fixed on `vulkan-fog-bloom`: the shadow projects on the default (simple)
path too, and bloom/FXAA run on the render-target path. See the last section.

Still open:
- `copyTargetToTexture()` supports an offscreen source only.

## Simple-path texgen + fog, bloom/FXAA (branch `vulkan-fog-bloom`)

Commit `d7334636c` + the changes on this branch (`vulkan-fog-bloom`, not
pushed). `nel/src/3d/driver/vulkan/` only.

### Object-space texgen in the simple shader

`SSimpleUniforms` grew from 336 to 1040 bytes (the ring slot follows
`sizeof`): a `mat4 modelView`, the user texture matrix per stage, the four
texgen modes, the four plane coefficients per stage and a `specularTexMtx`.
`buildVertexShader()` now:

- keeps `vUvN` as `vec4` and applies the stage's user texture matrix (the
  fragment shader divides by q for the projective shadow lookup);
- implements `ObjectLinear` (`texMatrix * objectPos`, the shadow receiver),
  `EyeLinear`, `SphereMap` and `ReflectionMap` with the same math and mode
  values as the mega VP (identity planes, like the GL driver);
- computes eye-space `modelView * objPos` for the eye-linear mode and for fog.

The shadow receiver's quad also needed `CMaterial::getZBias()`: the Vulkan
driver ignored it, so the quad lost the depth test against the terrain it
shadows. `computeViewport()` now applies the NeL depth range and
`mat.getZBias() * _OODeltaZ` to `minDepth`/`maxDepth`, like GL's
`glDepthRange`; `setupMaterial()` re-issues the dynamic viewport per material.
This is what made the shadow appear (same texgen code with and without it).

### Fog on both paths

The simple pixel shader has the same `applyFog()` as the mega PP (linear /
exp / exp2, `abs(eyePos.y / eyePos.w)`, same colour) fed per draw from
`enableFog`/`setupFog`/`setupFogMode`. Additive draws use the black fog
override, matching GL3. The `fogEnabled = 0` override in
`stageMegaObjectUBO()` is gone: `nlFogEnabled` is now the engine state, so the
two paths render the same fog.

### Render-target sampling orientation

Vulkan render-target images are stored top-down (viewport row 0 is the top),
while NeL texture coordinates have V = 0 at the bottom. The driver now sets a
per-stage flip mask (`SSimpleUniforms::RtFlipMask`, `CObjectUBOData::rtFlipMask`)
for stages bound to an **offscreen** texture (`ITexture::isOffscreenTexture()`,
i.e. the render target manager's effect targets), and both pixel programs flip
V for those stages. Without it the bloom/FXAA result was vertically flipped.
The shadow maps are `CTextureMem` render targets: the shadow map manager's copy
pass already produces them for the GL bottom-up convention, so flipping them
again moved the projected shadow off the receiver (verified).

### Bloom and FXAA

- `supportBloomEffect()` returns true.
- `stretchRect()` is `stretchRectImage()`: a scaled `vkCmdBlitImage` with NeL
  rect handling, suspending and resuming the open render pass. Every texture
  image now has `TRANSFER_SRC` usage so any texture can be a source.
- `CTextureOffscreen` already came from the render target manager; the target
  path handles it as before.
- The bloom blur draws with the engine's `TextureOffset` vertex program. The
  driver now skips a GLSL source that declares plain uniforms (Vulkan requires
  blocks) and uses the program's `nelvp` source, so that program compiles via
  the converter. The pixel stage for user VPs uses `nlRtFlipMask` (above) and
  the object vertex format now includes texgen stages and the texcoords a
  converted VP writes, so the four blur taps sample their own coordinates.
- User pixel programs are implemented (`compilePixelProgram` /
  `activePixelProgram`): the `glsl300esf` source is assembled with its UBO at
  binding 5, the sampler at binding 8, an output location and explicit varying
  locations; the shader replaces the mega pixel stage through a `UserPP` key.
  FXAA's own vertex program falls back to its `nelvp` source.
- The full-screen user-PP pass samples a render target, so its varying V is
  flipped at assembly time (documented in `addVulkanFragmentQualifiers()`).

### Verification (2026-11-11, run dir `ryzom_live_vk2_mega`, 1280x800, X11)

`Shadows = 1`, `Bloom = 1`, `FXAA = 1`, default path (no `NLVK_MEGA`),
`NLVK_VALIDATION=1`:

- build: `ninja -C build-vulkan -j 6 ryzom_client_dev lib/libnel_drv_vulkan.so`
  clean.
- `Vulkan: stretchRect 1280x800 -> 256x256 (scaled blit)` (bloom uses the
  blit, not the CDriverUser fallback).
- `Vulkan: compiled user pixel program 'glsl300esf/FXAA_PP/UBO'`;
  FXAA PP/VP available; bloom's `TextureOffset` VP compiles from `nelvp`
  (`... uses plain uniforms; using its nelvp source`).
- No validation errors: the only messages are the pre-existing "vertex
  attribute not consumed" pipeline warnings (duplicate limit). No black
  screen, no flip: UI, minimap and world are upright.
- Images in `/tmp/opencode/fogbloom/`:
  `simple.ppm.2400.png` is the pre-texgen/zbias control: avatar textured, no
  shadow under it; `zbias2400.png` is the same spot after texgen + zBias: a
  soft dark shadow blob left of the feet (this is the shadow-projection
  evidence). `finalcheck1200.png` is the current build with Shadows = 1 +
  Bloom = 1 + FXAA = 1 and validation on: upright, fogged, shadow visible as a
  soft patch under the feet (`finalcheck_feet.png`); `megafinal1200.png` is
  `NLVK_MEGA=1` with both effects (the post-restriction mega run is
  `mega_final_build.png`). The sun angle follows the in-game time, so the
  shadow is obvious only at some times of day (the zbias frame is a good one).
- `NLVK_MEGA=1` with bloom + FXAA also renders correctly (upright, fogged,
  validation-clean), so both paths still agree.

Known limitations:

- FXAA does not have its own support flag; `supportBloomEffect()` gates it, and
  the driver only compiles the `glsl300esf` profile. A pixel program without
  that profile disables itself with a warning.
- The user-PP varying flip assumes the program samples a render target (true
  for FXAA). A future user PP sampling a regular texture through the same
  varying would be flipped.
- The bloom blur's four taps now sample their own VP-written coordinates; the
  visible result is a soft glow. Its quality has not been compared against the
  GL3 driver side by side.
- The mega path's sky/backdrop still differs from the simple path (the clear
  colour shows where the simple path draws the sky texture); this predates
  this branch.

## Vulkan text corruption and colour parity (branch `vulkan-text-color`)

Started from `e407be0f8`. Two user-reported problems: garbled/missing text
everywhere, and every texture a little brighter than in the OpenGL client.

### Text: the driver was converting CTextureFont's Alpha atlas in place

`uploadTextureVulkan()` / `uploadTexturePixels()` called
`tex.convertToType(CBitmap::RGBA)` on every non-RGBA texture. `ITexture`
derives from `CBitmap`, so this rewrites the *source* bitmap: a
`CTextureFont` atlas (1 coverage byte per texel, `_Data[0][y*W+x]`) became a
4-byte RGBA buffer. `CTextureFont::copyGlyphBitmap()` / `repackAtlas()` /
`clearAtlas()` keep writing 1-byte coverage at 1-byte atlas offsets, so from
the first upload on every newly rendered glyph landed at a quarter of its
intended offset and in the wrong channels (R/G/B of neighbouring texels).
Strings rendered before the first upload looked fine; anything rendered later
(typed login name, chat, HUD updates) was garbled. That is why it "worked
partially".

Fix in `driver_vulkan_texture.cpp`:

- New `prepareTexturePixels()` only decompresses *compressed* textures in
  place (DXTC file data, one-time). Alpha / Luminance / AlphaLuminance / RGBA
  are now read where they are and expanded into the staging buffer by
  `convertRowToRGBA()`; the source texture is never mutated.
- `uploadTextureRegion()` uploads a clipped sub-rect of one mip level,
  preserving the rest of the image (`oldLayout` = the image's actual layout,
  `imageOffset` = rect origin).
- `setupTextureEx()` no longer re-uploads the whole texture on every
  `touchRect()`: a touched non-mipmapped texture uploads only
  `tex._ListInvalidRect`; a full invalidation (or a mipmapped texture, whose
  mip chain is rebuilt from the whole bitmap) still does a full upload. This
  is the font atlas hot path (one glyph tile at a time), and it also removes
  the stale-overwrite risk of the old full re-upload.
- `uploadTexture()` (shape bank / async texture manager) now creates the image
  without pixels and uploads exactly the requested rect and mip, instead of
  re-uploading the unsent rest of the bitmap.

Texture lifetime was checked but is NOT the cause here: the font atlas lives
in `CFontManager` for the whole run. The driver still has no texture free
path (`_TexturesVulkan` is only cleared at device shutdown), so freed
`ITexture`s leak their VkImage and a new texture allocated at a recycled
address can inherit a cached image. That is a separate piece of work; the
font texture is not affected.

### Colour: the swapchain was an _SRGB surface

The surface-format choice took the first `B8G8R8A8_UNORM` **or**
`B8G8R8A8_SRGB`. Mesa offers the sRGB variant first, so the swapchain was
`sRGB` and the hardware applied the sRGB transfer function to shader output
that was already in display space (GL3 renders into a non-sRGB default
framebuffer and never enables `GL_FRAMEBUFFER_SRGB`). `createSwapchain()` now
prefers `B8G8R8A8_UNORM`, falls back to `_SRGB` with a warning, and logs the
chosen format. Textures are still uploaded as `R8G8B8A8_UNORM` (GL3 uses
`GL_RGBA8`), so the whole path now matches GL3.

Pixel proof on the same UI element (login form CONNECT button fill,
1280x800): Vulkan before = RGB(65,143,65), Vulkan after = RGB(13,70,13),
GL3 = RGB(13,70,13). 143 is exactly the sRGB encode of 70 and 65 of 13, i.e.
the old Vulkan image was the GL3 image with one extra sRGB encode. The login
form's flat UI colours are pixel-identical between the fixed Vulkan driver
and GL3 (the five most common fill colours of the CONNECT button match
exactly); the remaining form-area difference is the 3D backdrop around it.

### Verification

Build: `ninja -C build-vulkan -j 4 ryzom_client_dev lib/libnel_drv_vulkan.so`
clean. `NLVK_VALIDATION=1` on the login screen and on the "already online"
error screen: only the pre-existing gamescope WSI
`VkSwapchainPresentModeInfoKHR` warnings and the pre-existing
`VK_EXT_surface_maintenance1` warning; no image/upload/texture VUID.

Images (`/tmp/opencode/textcolor/`):

- `before-login.png` (old driver) vs `after-login2.png` (fixed): same login
  screen. `before-login-user.png` / `after-login-user.png` are 4x crops of
  the login field: before, `mats007` is overlapped/garbled; after it is
  clean. Other labels happened to render before the first upload, which is
  why they looked fine in the old build.
- `gl3-form-host.png`: the GL3 driver on the same login form, the colour
  reference.
- `vk-ingame2.ppm.3000.png`: fixed driver, multi-line wrapped
  "account is already in online state" UI text, fully readable. This run was
  meant to reach the in-game chat/HUD, but the live account was taken by
  another agent at the same time ("You were disconnected by the server"),
  and the replacement run hit the 30 s online-state error. In-game
  chat/HUD verification is therefore still open; the same font atlas and
  material are used by the login UI and the HUD.

### Remaining

- In-game chat/HUD screenshot with the fixed driver (blocked by the live
  account collision; needs a dedicated account).
- Texture lifetime: implement per-texture driver objects through
  `ITexture::TextureDrvShare` so `~ITexture` frees the VkImage and the cache
  entry. Not the text bug, but the leak/stale-reuse is real.
- The Vulkan login form has a black backdrop where GL3 shows the 3D login
  scene (ring, logo, turtle). Pre-existing (the old driver screenshot is the
  same) and unrelated to this branch.

## Branch `vulkan-transforms-perf` (2026-11-11, unverified in game)

Base `e407be0f8`. Work stopped on request before any run on this branch: the
account coordination changed mid-session (this task's account is
`mats006`/`passDOSma`; `mats007` belongs to another agent) and game launches
were forbidden. **Everything below compiles but has not been run in game.**

### Reported problems and what was found

1. **Avatar equipment (head, hair, gloves) displaced.** Reproduced in the
   previous branch's captures:
   `/tmp/opencode/fogbloom/megacheck1200.png` (in game, avatar headless with a
   face and a hair item floating about one head above) and
   `/tmp/opencode/ryzomtest/final/skin2400.png` (character selection: same
   displacement in the 3D scene *and* in the UI portrait). The body itself is
   correct. In game the body is CPU-skinned (it has blend shapes); the
   equipment meshes are the ones that take the glsl3vi insert path
   (`GPU Skinning Insert` / `GPU Skinning Simple Insert`). The likely cause is
   therefore the GPU-skinning/insert path (bone palette, NlSkeleton staging or
   the insert snippet), which had only ever been checked for "textured", never
   for correct placement -- the GL drivers used for comparison run either the
   legacy OpenGL driver (no glsl3vi at all) or CPU skinning.

   Mitigation in this branch (correctness first): `supportVertexProgram(
   IProgram::glsl3vi)` now returns **false** by default, so the engine uses the
   known-correct CPU skinning path for equipment, NPCs and MRM geomorph. Set
   `NLVK_GPUSKIN=1` to exercise the insert path again. If the avatar is then
   correct, the insert path is confirmed as the cause and can be debugged in
   isolation.

2. **Huge tree branches too low.** Not reproduced on this branch (no runs).
   In `megacheck1200.png` and `ryzomtest/final/final.png` the giant trees
   render, but their canopies/branches are only ~20-30 m up instead of the
   ~200 m visible in the reference client. These trees are drawn with the
   wind-tree user vertex program (`glsl330v/MeshVPWindTree/UBO`, GLSL+UBO) and
   the MRM geomorph insert path. Both are suspects. With `glsl3vi` disabled the
   MRM geomorph path is out of the picture for a first A/B; if the branches are
   still low, the next step is to compare the same spot against
   `~/Projects/ryzom/ryzom_live` (legacy OpenGL driver, `Driver3D="OpenGL"`).
   The object UBO staging itself is shared with every other mesh and is
   correct for the ground/avatar, so a wrong MVP for just this class points at
   the per-program UBO (wind tree `NlWindTree`) or its attribute setup, not at
   `stageMegaObjectUBO`.

3. **Flicker during character selection.** Not investigated in game. New
   `NLVK_SCREENSHOT_EVERY=<n>` (`NLVK_SCREENSHOT=<prefix>.ppm
   NLVK_SCREENSHOT_FRAME=<first>`) dumps consecutive frames (`prefix.<frame>.ppm`,
   every `<n>` frames) so a 1-in-40 black frame can be captured and compared.
   The first suspect is the swapchain/render-target resume path
   (`beginMainPass(first=false)` uses `_ResumeRenderPass` with
   `initialLayout=PRESENT_SRC_KHR`), the second is a frame in which a texture
   upload or a deferred destroy stalls the queue. See the new batching below.

### Performance changes in this branch (compile-verified only)

- `beginFrame()` no longer calls `vkDeviceWaitIdle()`. Deferred texture and
  buffer destroys are tagged with the swap-buffer frame and released once they
  are two frames old (two frame slots are in flight, and the slot fence waited
  at `beginFrame()` proves the older one finished). This removes a device-wide
  stall whenever any texture or ring was recreated.
- `stageUserUB()` keeps the ring offset of a block that has not been
  `Touched` since it was copied. The 12 KiB `NlSkeleton` block is filled once
  per skeleton and read by every one of its skinned draws; before, it was
  memcpy'd into the ring for every draw. The cache is dropped when the ring
  grows and at every `beginFrame()`.
- First-time texture uploads are batched: newly created images cannot be
  referenced by an in-flight frame, so their copies are recorded into one
  upload command buffer and submitted once per `swapBuffers()` with a
  semaphore, instead of one command buffer + `vkQueueWaitIdle` per texture.
  In-place updates of an existing image (animated textures) keep the old
  synchronous path because an in-flight frame may still be sampling it.
  Staging buffers are freed when the batch fence signals at the start of the
  next batch. This targets the long loading times.
- `NLVK_STATS=1` logs every 120 frames: wall-clock ms/frame, drawers per frame,
  and the average CPU cost of the descriptor+draw and index-copy paths. Use it
  to produce the before/after numbers the task asks for (the old build did
  ~12.5 fps at 1280x800 on the moor with ~700 indexed draws/frame; that number
  includes a per-frame `vkDeviceWaitIdle` and the per-texture waits and is not
  a clean baseline).

### Next steps for the replacement agent

1. Build with the command in `AGENTS_wip.md` and run the reliable login guard
   with **`mats006`/`passDOSma`/slot 1**.
2. Run once without `NLVK_GPUSKIN` and once with it, capturing at the same
   frame (`NLVK_SCREENSHOT_SEQ=1`, `NLVK_SCREENSHOT_FRAME=2400`). The avatar's
   head/hair should be correct without it; if so, the insert path is the bug.
   The first thing to check there is the `NlSkeleton` block: dump the first
   16 bone matrices and the paletteSkin/weight of a head vertex on both paths
   (`fillGPUSkinBoneUBO()` is shared, so a difference would be in the driver's
   `stageUserUB()`/descriptor staging or in the `VK_FORMAT_R8G8B8A8_USCALED`
   attribute).
3. Compare the giant-tree spot with `~/Projects/ryzom/ryzom_live` (its
   `client.cfg` uses `Driver3D="OpenGL"`); take the same frame from both and
   measure the canopy height. If the wind-tree VP is at fault, look at
   `meshvp_wind_tree.cpp` (the GLSL UBO body is shared with GL3) and at the
   `NlWindTree` user UBO staging.
4. For the flicker, set `NLVK_SCREENSHOT_SEQ=1 NLVK_SCREENSHOT_EVERY=1
   NLVK_SCREENSHOT_FRAME=2000` during character selection and diff consecutive
   frames. Also try `NLVK_STATS=1` to see whether the black frame coincides
   with a device stall.
5. Validate the new upload batching with `NLVK_VALIDATION=1` (layout
   transitions are the risk). If anything looks wrong, the fallback is a
   one-line change: pass `false` from `uploadTextureVulkan()` to
   `uploadTexturePixels()` to get the old synchronous path back.

## Integration of `vulkan-transforms-perf` + in-game verification (branch `vulkan-integrate-verify`)

Merge commit `4089cdd6b` (`git merge --no-ff vulkan-transforms-perf` on top of
`4b7aba749`). The only textual conflict was `driver_vulkan_texture.cpp/.h`; both
sides had rewritten the uploader.

### What was kept from each side

Main (font/colour fix), unchanged semantics:
- `prepareTexturePixels()` only decompresses compressed formats; an
  uncompressed bitmap is read where it is, so `CTextureFont`'s Alpha atlas keeps
  its one coverage byte per texel (no `convertToType()` on the source).
- `convertRowToRGBA()` / `copyTextureRect()` expand each row from the bitmap's
  own format (RGBA / Alpha / Luminance / AlphaLuminance) into tightly packed
  RGBA8, and the staging buffer is filled whole-mip by whole-mip.
- The partial-rect path: `uploadTextureRegion()` (touchRect / streaming),
  `setupTextureEx()` sends only the touched rects, or the whole mip chain when
  the texture was fully invalidated or is mipmapped.

Branch (`vulkan-transforms-perf`):
- Batched first-time uploads: one upload command buffer per frame, submitted at
  `swapBuffers()` before the frame, the frame waits on its semaphore
  (`VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`), so a new texture costs no
  `vkQueueWaitIdle`.
- Deferred destroys tagged with the swap-buffer frame (no `vkDeviceWaitIdle` in
  `beginFrame`).
- User-UBO ring cache in `stageUserUB()`, `NLVK_STATS`, `NLVK_GPUSKIN`
  (glsl3vi opt-in, CPU skinning default) and `NLVK_SCREENSHOT_EVERY`.

Resolution: the uploader keeps the new per-format row expansion, which is
independent of where the copy is recorded. `beginTextureCopy()` /
`endTextureCopy()` hold the whole decision in one place: a copy for an image no
in-flight frame can reference (a brand-new image, or one that already has a copy
in the current batch) is recorded into the per-frame batch; anything else (an
in-place update of an image a frame may be sampling: animated textures, font
glyph tiles) stays in the old synchronous one-off command buffer. A resource
being retired waits for the upload-batch fence before it is freed.

### Fixes that came out of the verification (same branch)

1. `NLVK_SCREENSHOT_EVERY` wrote every dump to the same file (only
   `NLVK_SCREENSHOT_SEQ` used the `prefix.<frame>.ppm` name). Both now name the
   file after the frame, and `NLVK_SCREENSHOT_EVERY` overrides the 600-frame
   period of `SEQ`.
2. `VUID-vkCmdDraw-None-09600` (pre-existing, also on `4b7aba749`): an image
   created without pixels (`uploadTextureVulkan(..., upload=false)`, the
   streaming path) only ever had mip 0 transitioned by `uploadTextureRegion()`,
   while the view and the sampler cover every mip. The remaining mips stayed in
   `UNDEFINED`, and every draw sampling the texture was flagged. The image is
   now transitioned to `SHADER_READ_ONLY_OPTIMAL` for all mips when it is created
   without pixels.
3. `VUID-vkDestroyBuffer-buffer-00922` (introduced by the branch's deferred
   destroy): a resource retired *between* frames can be referenced by the most
   recent submission, which belongs to the other frame slot, and that slot's
   fence is not the one waited in `beginFrame()`. Retiring after three swap
   frames (`tag + 3 <= counter`, and nothing at all during the first three
   frames) is the first provably safe point. Reproduced at the index-ring growth
   during startup, gone after the change.

### In-game verification (mats006 / passDOSma, slot 1 = Danilli)

Run directory `/tmp/opencode/iv/run-vk` (data/cfg symlinked to
`~/Projects/ryzom/ryzom_live`, `Driver3D="Vulkan"`, 1280x800, `Shadows = 0`,
`InterfaceScaleAuto = 1`). Runner scripts: `/tmp/opencode/iv/{guard,run,gl_run}.sh`.
Screenshots in `/tmp/opencode/iv/evidence/` (with a `README.txt` naming each
one; the PPM bursts are in `/var/tmp/iv/`).

**Avatar.** The base build (`4b7aba749`, `glsl3vi` on by default) at character
selection is exploded: legs on the floor, one arm floating, the clothes above
the head (`charsel_base_build.png`). With `NLVK_GPUSKIN=1` the merged build
reproduces exactly that (`charsel_merged_gpuskin.png`), so the insert path is
confirmed as the cause of the displaced equipment. With the new default
(`glsl3vi` off, CPU skinning) the avatar is coherent: head with red hair,
shoulders, arms and hands at the sides, legs and shoes in place
(`charsel_merged_cpu.png`, in game `ingame_merged_cpu_runA.png`).

Still wrong in game and at character selection: a large beige/tan
triangulated smear around the body, as if a few vertices of a clothing mesh
were skinned by a wrong bone or read with the wrong vertex layout
(`charsel_merged_cpu_crop.png`, `ingame_merged_cpu_crop.png`). It is present
with and without `NLVK_GPUSKIN`, so it is not the insert path. It is
Vulkan-only: the same merged binary and the same data render the same
character perfectly with the OpenGL driver (`p_gl2-wide.png`), and the legacy
`~/Projects/ryzom/ryzom_live` client shows the same clean avatar. Not a
regression of this merge (the merge only touches texture upload and deferred
destroys), but it is the next avatar bug to chase.

Also Vulkan-only, same "some meshes are not drawn" family: the in-game HUD in
Vulkan only draws the chat and the missions window, while the OpenGL driver of
the same build draws the top status bar, the quickbar, the compass/minimap and
the right-hand "Windows/Actions" panel (`p_gl2-wide.png`). Char selection and
the login form draw their full UI.

`The Bone for the Head is missing` / `The Bone for the Chest 'Bip01 Spine1' is
missing` in the log are **not** driver-related: the same build logs them with
the OpenGL driver (182 times) and still renders the avatar correctly. They come
from `CCharacterCL::computeSomeBoneId()` and only disable the name/look-at
attachments.

**Giant trees.** Same spot, same camera (default `CameraDistance = 8`, no
zooming): the merged Vulkan render and the legacy OpenGL client agree on the
geometry. `trees_zoom_cmp.png` shows the same crop of both: the giant mushroom
cap, its stem, the giant trunk behind it and the rock spire in the distance are
at the same screen positions and sizes in both. Cross-correlating the edge maps
of the two full frames (320x200, search +-10 px) puts the best match at zero
shift: NCC 0.309 for the CPU-skinning capture and 0.305 for
`NLVK_GPUSKIN=1` against the reference, and 0.443 for this build's OpenGL
driver against its Vulkan driver. So the "branches only 20 m up" of
`megacheck1200.png` is not reproduced on this branch, and the reference client
draws this spot the same way. The visible difference is fog/haze: the Vulkan
render is much hazier, so the distant purple canopies wash out to white
(`trees_side_by_side.png`). `NLVK_GPUSKIN=1` does not change the trees.

**Flicker.** `NLVK_SCREENSHOT_EVERY=1` at character selection, frames 3000-3125
(126 consecutive driver dumps, `/var/tmp/iv/g_charsel.ppm.*`, analysed with
`/tmp/opencode/iv/flicker.py`): per-frame mean luminance 51.26..52.17 (28.5% of
the pixels are the black UI side panel in every frame), no frame below 51, no
sudden drop against its neighbours. There is no black frame in the driver
output. Note the burst costs ~450 ms/frame (fence wait + readback per dump).

**Performance** (`NLVK_STATS=1`, 1280x800, RADV, ~1200 indexed draws/frame,
Debug build):

| | in game | char selection | connect -> in game |
|---|---|---|---|
| base `4b7aba749` (glsl3vi on) | 31.0 ms/frame (32.2 fps) | - | 24-25 s |
| merged, default (CPU skin) | 31.7-32.4 ms/frame (31 fps) | 8.3 ms/frame (120 fps) | 24 s |
| merged, `NLVK_GPUSKIN=1` | 31.1-36.9 ms/frame (30 fps) | - | - |

The in-game numbers are 600-frame wall-clock deltas from the log (`Vulkan:
frame N`), the other column is the `NLVK_STATS` report every 120 frames; both
builds render the same scene at ~1200 indexed draws/frame. The base and the
merged build are the same within noise. The driver's
per-draw CPU cost is 3.1 us descriptor+draw + 0.2 us index copy, i.e. ~4 ms of a
~31 ms frame, so the frame is GPU/engine bound and the removed device waits do
not show up here. Loading time is identical (24-25 s); texture creation is not
the bottleneck of that phase in a Debug build. Remaining sync hot spots: every
in-place texture update (animated textures, one per font glyph tile) still does
`vkDeviceWaitIdle` + submit + `vkQueueWaitIdle`; batching those needs an
ordering rule against the previous frame's fence.

**Validation** (`NLVK_VALIDATION=1`, in game, 100 s): clean except
`VUID-VkSwapchainPresentModeInfoKHR-swapchainCount-*` (gamescope WSI, known) and
`VUID-vkCreateDevice-ppEnabledExtensionNames-01387` (gamescope's
`VK_EXT_surface_maintenance1`, known). The two real VUIDs found and fixed are
described above.

### Left to do

- ~~The beige smear on the avatar~~: fixed by the per-frame vertex ring, see the
  memory section below. It was not a vertex-format problem: the avatar's
  CPU-skinned vertices were rewritten into the same mapped VkBuffer that draws
  of unsubmitted frames still referenced.
- ~~In-game HUD windows missing~~: same cause, fixed by the same change.
- Fog/haze is much stronger in Vulkan than in the OpenGL client at the same
  spot (`trees_side_by_side.png`), so distant vegetation washes out; the scene
  is also darker/warmer than GL3.
- In-place texture updates still stall the device (see above).
- `WM_DELETE_WINDOW` shutdown: the Vulkan client starts releasing but then hits
  `vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed` while recreating the
  swapchain during teardown and has to be killed. Use Escape -> Quit Ryzom ->
  Quit Now for a clean logout (the logout packet is only sent on that path).

## Memory, DXTC and the dynamic-buffer races — fixed (branch `vulkan-memory-hud`)

The game now uses >3 GB resident RAM, roughly 3x the original client. Suspects:
- `_TexturesVulkan` never evicts textures (every distinct ITexture keeps its
  VkImage/ImageView/Sampler/Memory for the whole session; also a stale-texture
  hazard when an ITexture address is recycled);
- per-draw uniform/index rings only grow (`_UniformRingSize *= 2`, 16 MB index
  ring) and old buffer allocations may be kept;
- deferred-destroy lists and the upload-batch fence paths;
- two frames in flight with full descriptor pools per frame (4096 sets each).
Validate with NLVK_VALIDATION=1 and measure RSS at fixed points (login,
character selection, in game after N minutes, after moving between areas).
Implement texture eviction (`ITexture::TextureDrvShare` / ITextureDrvInfos) and
check for double allocations. This is a task that can run concurrently with
rendering work.

## Measuring FPS in game (user tip)

The in-game configuration window (opened/closed with the `u` key) shows an FPS
counter in its top-right. Use it to cross-check `NLVK_STATS=1` numbers; the
debug build's Lua scripting can open the window if xdotool is inconvenient.
- `valgrind` is installed (user). For the RSS issue prefer `--tool=massif`
  (two snapshots: login screen vs after N minutes in game) and
  `--leak-check=full` with suppressions; expect heavy Mesa/RADV/X11 noise.
- User observation 2026-11-11: a running client showed ~1.1 GB RES (down from
  >3 GB) at ~12 FPS. Confirm which build that was and record the RSS at each
  measurement point in the memory work's report. FPS is explicitly not a
  priority right now; correctness first.

Commit `596557f25`. The >3 GB RSS, the beige smear around the avatar and the
missing HUD windows all came from the same class of bug: per-draw data was
written into buffers that recorded draws of the current or previous frame still
referenced.

**Dynamic buffers.** The uniform and index rings restarted at offset 0 every
frame while the previous frame (2 frames in flight) could still be reading its
blocks, so a draw could pick up another draw's uniforms/indices. Each ring now
has one half per frame. CVertexBuffers the CPU rewrites (`FullStream`,
`SmallStream`) go through a new per-frame vertex ring, and `FullRewrite`
(skinning, water) keeps two persisted copies that alternate when the current
one was drawn with. This is what made the whole HUD draw again (the UI issues
several `drawQuads` batches per frame into one engine VB) and put the avatar's
clothing and hair back on the body.

**Textures.** The driver now implements `ITextureDrvInfos` / `CTextureDrvShare`
like GL3: one `CTextureDrvInfosVulkan` per texture (or per share name for
shared textures), created on first use and destroyed with its last `ITexture`.
A recycled `ITexture` address can no longer show a stale image, shared files
upload once, and `profileAllocatedTextureMemory()` / `getUsedTextureMemory()`
work. DXTC1/1A/3/5 bitmaps are no longer decompressed to RGBA in place on
upload: when the device reports `textureCompressionBC` they are uploaded as
BC1/BC2/BC3 (raw mip chain, block-aligned region path for
`CAsyncTextureManager`), which removes the 4-8x CPU expansion and 4x device
memory of the RGBA upload. The uncompressed fallback keeps the old conversion.

**Sizes.** Uniform ring 128 MB -> 4 MB and index ring 64 MB -> 2 MB (grown on
demand, per-frame halves), descriptor pools sized for the 4096-set layout
instead of 64k samplers, and the frame log prints the live texture memory.

Measurements (`mats005`, gamescope 1280x800, Debug, RSS from
`/proc/<pid>/status` every 2 s; VRAM from
`/sys/class/drm/card1/device/mem_info_vram_used`, which includes the ~1.45 GB
desktop baseline):

| point | before (`c9e6828cf`) | after (`596557f25`) | GL3, same build |
|---|---|---|---|
| login screen | 224 MB | 220 MB | - |
| character selection | 452 MB | not re-measured | - |
| in game, 1 min | 3380 MB | 1143 MB | - |
| in game, 5 min | 3380 MB | 1127 MB (peak 1158 MB) | 965 MB |
| city, slot 0 | not measured | 1300 MB | - |
| VRAM in game | 3090 MB | 1740 MB | 1950 MB |

The character-selection point on the final build was not captured (the account
was still online when the run started); the final build's login number is the
same as before, and the in-game/city points above are final-build numbers. The
remaining +160 MB over GL3 is the two-copy FullRewrite VBOs, the three rings,
the descriptor pools and driver-internal objects; RSS is flat over five
minutes and over a city load (no per-frame growth).

**Visual verification.** In game and in the city the HUD is complete (stats,
action bar, compass, missions, windows panel, chat) and the avatar, clothing
and hair are correct; screenshots `/tmp/opencode/mem/out/vk-smoke.png` and
`vk-city-2400.png` next to the GL3 reference `gl3-final-gshot.png`. The city
interior was checked with `NLVK_MEGA=1` (slot 0, Matsfive): the log reports
`Vulkan: LightMap mega path active: 1 lightmaps in 1 pass(es), 7 per pass`, and
the wooden interior/NPC are lit and textured.

**Validation** (`NLVK_VALIDATION=1`, final build, city + mega path): only the
known gamescope `VkSwapchainPresentModeInfoKHR` and
`vkCreateDevice-ppEnabledExtensionNames` VUIDs.

Still open:
- Blitting from a BC texture in `stretchRect()` is untested (FXAA/bloom only
  use RGBA render targets today).
- The FullRewrite two-copy rotation assumes one write per buffer per frame
  (true for the skin manager); a third write to the same VB in one frame could
  still be seen by an earlier recorded draw.
- In-place (non-first-time) texture updates still do a device wait + submit.

## Window close, present instrumentation, preview check (branch `vulkan-shutdown-flicker`)

Worktree `f1fb99/vulkan-shutdown-flicker`, based on `cc519718f`.

**Closing the window now exits with status 0.** Three separate bugs were in the
way:

1. `CVulkanEventEmitter::processMessage()` called `XGetWindowAttributes()` on
   the window for every event, at the top of the function. When the WM destroys
   the window (`xdotool windowclose`, a force close) the next event made that
   call fail; Xlib's default error handler then *exits the process* with an X
   error, which looked like a crash during teardown. The call is now wrapped in
   a scoped handler that ignores protocol errors, and `DestroyNotify` for the
   game window is now treated like `WM_DELETE_WINDOW` (previously it was taken
   for "XIM server crashed" and the client kept looping against a dead surface).
2. `recreateSwapchain()` could be called after the surface was gone. It
   destroyed the swapchain, `createSwapchain()` failed on
   `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` (VK_ERROR_SURFACE_LOST_KHR,
   -1000000000), and the frame loop kept calling `vkAcquireNextImageKHR` on a
   null swapchain. The driver now checks that the X window still exists *before*
   touching the surface (no more capabilities call on a dead window), marks the
   swapchain lost when recreation fails, and `beginFrame()`/`swapBuffers()`/
   `clear2D()` become no-ops once `_ShuttingDown`/`_SwapchainLost` is set.
3. `IDriver::release()` hung forever deleting GPU program driver info: the
   Vulkan driver inserted a NULL into `_GPUPrgDrvInfos` and never wrote the new
   `CProgramDrvInfosVulkan` back into the list, so the `while (begin() != end())
   delete *it;` loop deleted NULL forever. Fixed at all four creation sites
   (user VP, insert VP, GLSL VP, user PP). The client only hung once a user
   program existed, i.e. at character selection with `NLVK_GPUSKIN=1` or in
   game with the mega path; the login screen hid it.

Teardown order was also tightened: `release()` waits for the device while the
window is still mapped, then unmaps (like GL3) and stops all further presents;
`shutdownVulkan()` destroys the swapchain before the presentation semaphores
and the device, destroys render-target framebuffers still in the deferred
destroy list (validation had reported two leaked `VkFramebuffer` at
`vkDestroyDevice`), closes the XIM and destroys the X window/closes the display
under the same non-fatal X handler. `isActive()` no longer lets an X protocol
error kill the process.

Verified (mats006, gamescope 1280x800, Debug):

| run | close | result |
|---|---|---|
| login screen, `NLVK_VALIDATION=1` | WM_DELETE | exit 0, no VUID, no leak |
| login screen | `xdotool windowclose` (window destroyed) | exit 0, no capabilities error, no X error |
| character selection, `NLVK_GPUSKIN=1` | WM_DELETE | exit 0 |
| in game, `NLVK_VALIDATION=1` | WM_DELETE | exit 0, only the known gamescope WSI VUIDs, no leaks |
| in game | Escape -> Quit Ryzom -> Quit Now | exit 0 |

No `*.dmp` was produced by any of these runs.

**Present instrumentation (`NLVK_PRESENT_LOG=1`).** One line per presented
frame with the frame id, CPU interval since the previous present, swapchain
generation, acquired image index, acquire/fence/submit/present `VkResult`, and
the cumulative skipped-frame count; skipped frames and swapchain recreations
get their own lines with the reason. All logging is gated by the env var.

Character-selection flicker: not reproduced, and no evidence of a driver-side
skip was found. The present log of a run that logins and ends at character
selection showed **5564 presented frames, one swapchain generation,
acquire/present VK_SUCCESS on every frame, zero skipped frames, zero
out-of-date/suboptimal results**; the 364 consecutive
`NLVK_SCREENSHOT_EVERY=1` dumps taken at character selection (frames 5200-5563)
kept a mean brightness within 154.5..155.6 (a black frame would be ~0). A
separate login-screen run (2946 frames) was equally clean, and in game 517
more frames showed the same result. The remaining explanation is the
compositor/observation (gamescope), or an artifact of a pre-fix build; with
the fix the driver never skips or recreates at character selection. The
instrumentation stays in the driver so the next report can be checked quickly.

**Character-selection preview.** Captured with the default build (CPU
skinning) and with `NLVK_GPUSKIN=1`: both render the full-body avatar cleanly
(`charsel-cpu.png`, `charsel-gpuskin.png`); the small portraits are fine in
both. The insert path only changes the idle pose/arms in these captures, no
smear or displaced geometry, so the reported preview artifacts are not present
on this build.

**Loading-screen text.** The loading messages are drawn at the top right and
the sampled ones are complete ("Putting masks on all the Zoraïs...",
"Singing ryzker sleeping songs...", "Oh my god, look, a flying Yubo!",
"Teasing Frahars..."). The version line
(`DEV Omega / v26.11.422 #cc519718f`) is right-aligned at x=1.0 and its last
glyph sits on the last pixel column, so it looks clipped at the screen edge;
that is `progress.cpp`'s anchor/layout, not glyph atlas clipping (all glyphs
render and the message line below has a margin). Not changed.

Run scripts and evidence from this session: `/tmp/opencode/sd/` (out/*.log,
charsel.png, gpuskin.png, ingame2.png, flicker dumps in `/tmp/opencode/sd` are
in `/var/tmp/sd/`).

Still open:
- The FullRewrite two-copy rotation assumes one write per buffer per frame
  (true for the skin manager); a third write to the same VB in one frame could
  still be seen by an earlier recorded draw.
- Depth buffer is shared by all swapchain images and by both frames in flight;
  correct today only because the render pass clears it at the start of every
  frame, but two in-flight frames still write the same depth image. Making it
  per image (or per frame) would remove the race.
- The character-selection/"present-level" black frame is not reproducible from
  the driver side; check with `NLVK_PRESENT_LOG=1` if it is seen again.


## Character-selection flicker — presumed fixed (user, 2026-11-11)

The user reports the flicker was already gone after the RAM/dynamic-buffer fix
merge (`cc519718f`). Keep `NLVK_PRESENT_LOG=1` for the next sighting; no action
otherwise. The remaining open items are being worked by two agents:
- `vulkan-color-fog` (mats005): fog/colour parity with GL3 and ForceDXTC of RGBA
  sources.
- `vulkan-texperf` (mats006): in-place texture-update device waits, BC blits in
  stretchRect and remaining per-frame sync/stalls.

## Pinning the time of day for comparisons (user tip)

Ryzom's day/night cycle is about 1 hour of wall time. In a debug build
(`-DFINAL_VERSION=OFF`) `/setLightHour 12` (or the equivalent Lua call) pins the
client at noon. Use it for colour/fog/transparency comparisons so both drivers
render the same lighting, and take captures at the same pinned hour.

## Texture upload performance + BC stretchRect (branch `vulkan-texperf`)

Worktree `f1fb99/vulkan-texperf`, based on `fb6e5b5e5`, branch `vulkan-texperf`.
`nel/src/3d/driver/vulkan/` only; GL/GL3 drivers untouched. Verified in game
with `mats006` (slot 1, Danilli).

### The stall that was removed

`uploadTexturePixels()` / `uploadTextureRegion()` (in-place updates: animated
textures, and **one per font glyph tile**, i.e. ~20-70 per frame in game) did:

    vkDeviceWaitIdle(device)  ->  allocate cmd buffer  ->  vkQueueSubmit
    ->  vkQueueWaitIdle       ->  free cmd buffer, destroy staging buffer

per update. Every one of those waits for the previous frame to finish on the
GPU, so the CPU and the GPU were fully serialised around each texture update,
and each update allocated a `VkBuffer` + `VkDeviceMemory`.

### The new path

One **upload command buffer per frame slot** (`_UploadCommandBuffers[2]`,
`_CommandBuffers[2..3]`) is reset in `beginFrame()` (after the frame fence for
that slot has been waited) and submitted **in the same `vkQueueSubmit` as the
frame, ahead of it**:

    vkQueueSubmit({ uploadCmd[frame], frameCmd })   // one fence, no semaphore

* The copies and their layout transitions therefore execute before any draw of
  the frame; the frame sees every texture in its final layout and content.
* The first barrier of each copy (`SHADER_READ`/`TRANSFER_WRITE` ->
  `TRANSFER_WRITE`, src stage VERTEX|FRAGMENT|TRANSFER) is ordered against the
  *previous* frame's sampling by submission order on the same queue, which is
  what replaces the `vkDeviceWaitIdle()`. No CPU wait remains at all, and the
  frame's own fence covers the uploads.
* Updating the same image twice in one frame is just two copies in the same
  buffer, in recording order - no wait is needed (the task's "wait when the
  same texture is updated twice in a frame" case does not arise, because the
  copies are not interleaved with the frame's draws).
* Copies requested between two frames (the engine can set textures up outside a
  frame) are queued in `_PendingTextureCopies` and replayed by the next
  `beginFrame()`.
* Staging: new **host-visible staging ring** (`_StagingRing`, 8 MiB, one half
  per frame in flight, 16-byte aligned slices, grows by doubling up to 64 MiB).
  No more per-upload `VkBuffer`/`VkDeviceMemory` allocation and no more
  `_UploadStaging` list; uploads that do not fit in a ring half get one
  dedicated buffer that is retired with the frame (`deferDestroyBuffer`).
* `_UploadFence`, `_UploadDone`, `beginTextureCopy()`, `endTextureCopy()`,
  `isImageInUploadBatch()` and the extra `vkWaitForFences(upload)` in
  `beginFrame()` are gone. Remaining device waits in the driver are startup,
  shutdown, swapchain recreation and the one-shot `getWhiteTexture()`.

Important detail found the hard way: the upload command buffer **must be per
frame slot**. With one buffer for both slots, frame N+1 reset it while frame
N's submission (which contains it) could still be executing -> immediate
`VK_ERROR_DEVICE_LOST`. The rule is the same as for the draw command buffers.

### BC in `stretchRect()`

`vkCmdBlitImage` cannot read or write a block-compressed image (no driver
reports `BLIT_SRC`/`BLIT_DST` for BC formats), and it also requires both images
to have the same format. `stretchRectImage()` now:

* returns false, with a one-time warning, when the formats differ (the engine's
  `CDriverUser::stretchRect()` then draws its textured quad, which handles any
  format);
* uses `vkCmdCopyImage` when both sides are compressed and the rects are
  identical and block-aligned (a real BC copy);
* returns false when a compressed image would have to be scaled;
* keeps the filtered blit for uncompressed formats (bloom/FXAA).

`NLVK_TEXTEST=1` runs a one-shot self-test at frame 2 (inert without the
variable): it builds a 64x64 solid-red DXT1 texture, asks `stretchRect()` for a
1:1 copy and for a scaled copy, reads the destination block back and checks it.
In game: `same size -> true, scaled -> false` and
`PASS, destination block colour0 = 0xf800`.

### Measurements (RADV 840M, Debug, 1280x800, same client binary, driver `.so`
swapped; `NLVK_STATS=1`; another agent's client was running on `mats005` during
all runs, so the absolute numbers are loaded, the comparison is fair)

In game, mean over `NLVK_STATS` windows with >1000 draws/frame:

| build | ms/frame | fps | texture copies | upload submits | device time in upload waits |
|---|---|---|---|---|---|
| `fb6e5b5e5` (before) | 35.9 / 35.1 (two runs) | 27.8 / 28.5 | 25/frame | 25/frame (one each) | **2.1 ms/frame** |
| `vulkan-texperf` | 29.0 / 30.0 / 30.3 (three runs) | 34.5 / 33.3 / 33.1 | 20/frame | 0 | **0** |

=> ~16% shorter frames, ~+19% fps; the in-game `u` window counter reads
**FPS: 33** (vs the 33.1 measured by `NLVK_STATS` in the same run).
Per-draw CPU cost is unchanged (3.0-3.1 us descriptor+draw, 0.2 us index copy),
so the gain is entirely the removed device waits/serialisation.

### Verification

* Build: `ninja -C build-vulkan -j 4 ryzom_client_dev lib/libnel_drv_vulkan.so`
  clean (the usual warnings only).
* In game, `NLVK_VALIDATION=1 NLVK_TEXTEST=1 NLVK_STATS=1`: no VUIDs apart from
  the known gamescope `VkSwapchainPresentModeInfoKHR` ones, the known
  `VK_EXT_surface_maintenance1` one and the benign "vertex attribute not
  consumed" pipeline warnings. `TEXTEST: PASS`.
* Rendering unchanged: HUD complete (status bar, quickbar, compass, missions,
  chat, windows panel), font glyphs crisp (the atlas is the hot path this
  change touches), avatar textured. Screenshots:
  `/tmp/opencode/texperf/out/{final-perf,before-u}-*.png`, `after3-6300.png`
  (the `u` window), `after-val-*.png` (login screen).
* Clean logout (`Quit`/WM_DELETE) in every run; no `*.dmp`.

### Remaining hot spots (measured)

1. **Per-draw descriptor set**: one `vkAllocateDescriptorSets` +
   `vkUpdateDescriptorSets` per draw costs **3.0-3.1 us/draw**, ~3.5 ms of a
   29 ms frame at 1170 draws. Caching sets per (material, texture set, light
   table) would be the next CPU win.
2. **GPU**: ~1170 draws/frame with the landscape and vegetation; the frame is
   GPU bound (the CPU side is ~5 ms of the 29 ms).
3. **Texture upload traffic** is ~20 copies/frame and ~0.02 MiB/frame in game -
   negligible now, but the staging ring has to grow to 64 MiB during loading
   (single large textures of 1.4 MB plus lightmap tiles); in-game usage stays
   around 32 MiB/half.
4. **Depth buffer** is still shared by both frames in flight (pre-existing).

### Still open

- `ForceDXTC` compression of RGBA source textures is still a no-op.
- Fog/colour parity with GL3 (the Vulkan scene is darker and warmer).
- The FullRewrite two-copy rotation assumes one write per buffer per frame.
- Depth buffer shared by both frames in flight.
- The character-selection "black frame" is not reproducible from the driver.

## Descriptor-set caching / perf audit — in progress

Agent `vulkan-descriptors` (`mats006`, worktree `f1fb99/vulkan-descriptors`,
base `a463c5e44`) is working on:
- caching the per-draw descriptor sets (3.0-3.1 us/draw, ~3.5 ms of a 29 ms
  frame at ~1170 draws) in a frame-safe way (2 frames in flight), with
  before/after `NLVK_STATS` numbers;
- confirming the CPU/GPU balance and whether more CPU-side work is worthwhile;
- the staging ring's 64 MiB loading peak (~0.02 MiB/frame in game);
- an audit: the shared-depth-buffer item is already fixed on main (`fb6e5b5e5`,
  one depth image per swapchain image), ForceDXTC and fog/colour parity belong
  to `vulkan-color-fog`.


## Fog/colour parity and ForceDXTC (branch `vulkan-color-fog`, commit see log)

Verified in game on `mats005` (character `Kykikos`), 1280x800, with the debug
client's lighting pinned at noon via `/setLightHour 12` typed into the chat
(the day/night cycle is ~1 h; without pinning, two runs see different light).
Vulkan captures are `NLVK_SCREENSHOT` frame dumps; the GL3 reference is the
window captured with `import` under gamescope. Evidence in
`/tmp/opencode/fog/out/` (`glfinal-shot1.png`, `vkval-300.png`,
`vkmega-final.png`, `vkdxtc2.png`, `vknodxtc.png`, `top-compare2.png`).

What made Vulkan darker and warmer (three separate bugs):

1. **Mega light table never reached the shaders.** It was staged into a
   dedicated host-visible VkBuffer bound at descriptor binding 1, but the
   vertex programs read zeros from it (a white-table test and a shader-side
   `li.diffuse = 1` test both lit the scene; the CPU side showed a valid sun
   entry). All mega/user-VP meshes (trees, mushrooms, vegetation) therefore
   received only the ambient/self-illumination term. The table is now staged
   in `_MegaLightTableEntries[NL_VK_MEGA_MAX_LIGHT_TABLE]` and uploaded at
   draw time through the per-frame uniform ring, the same path as the
   camera/object/material blocks; the separate buffers are gone. This is the
   fix that makes the scene match GL3.
2. **Simple-path lighting did not match the mega vertex program.** It
   multiplied the ambient/self-illumination by the material diffuse, ignored
   the material emissive, and had no clamp or `(f + (f>>7))/256` light-factor
   expansion. It now computes `diffuse * lights + selfIllumination` exactly
   like the mega VP, with self-illumination staged like the mega object UBO
   (global + per-light ambient, times material ambient, plus emissive).
3. **Simple-path TexEnv ignored the stage constant colour** and used `vColor`
   for both diffuse and constant. This painted the sky fog dome (which gets
   the fog colour through `setInstanceConstantColor`) white. Added
   `texEnvConstant[4]` to `SSimpleUniforms`/the GLSL block and staged
   `CMaterial::_TexEnvs[stage].ConstantColor`.

The final colour path itself was already right: B8G8R8A8_UNORM swapchain (no
sRGB), GL3 renders to a non-sRGB 24 bpp visual, and
`setMonitorColorProperties` is a no-op in both drivers. UI panels are now
pixel-identical (MAD 0.03), which proves the output/gamma path.

Fog: `setupFog` parameters and colour were already identical between the
drivers; both use the GL3 linear/exp/exp2 maths with `abs(ecPos.y/ecPos.w)` and
the black-fog override for additive blends/passes. The old "mega object UBO
fogEnabled = 0" override was already gone; `NLVK_MEGA=1` and the simple path
now agree within ~1.6 units per region (the remaining canopy MAD is wind
animation phase, not colour).

**ForceDXTC.** There is no engine RGBA->DXT compressor in this tree:
`CBitmap::convertToType(CBitmap::DXTC5)` only rewrites existing DXT1 data
(`convertToDXTC5` returns false for RGBA), and
`CHLSColorTexture::compressBlockRGB` is private, RGB-only (it leaves the alpha
block untouched) and tied to HLS recolouring. GL3's ForceDXTC path is not a
compressor either: it passes an RGBA pointer to `glTexImage2D` with a
`GL_COMPRESSED_*` internal format, which GL does not define as a compression
request. The Vulkan driver now has a small BC3 (DXT5) encoder in
`driver_vulkan_texture.cpp` (8-value alpha palette, bounding-box 565 colour
endpoints, clamped edge blocks), used when `forceDXTCCompression(true)` is
set, the bitmap is RGBA with `ITexture::Auto`/`allowDegradation()`, and the
device supports `textureCompressionBC`. Mips are built as RGBA and each level
is compressed on upload; rectangle updates fall back to a full recompress.
Verified with `ForceDXTC = 1;` in the run dir's `client.cfg` (backed up and
restored): 18 RGBA textures uploaded as BC3, validation clean, and the
non-DXTC capture differs only by expected DXT quantisation (ground MAD ~3).
The encoder is bounding-box quality, not a cluster-fit encoder.

Before/after region means (Vulkan vs GL3, pinned noon; R,G,B):
- before: canopy (67.7,64.9,59.3) vs (109.6,101.3,97.2); trunk
  (53.6,42.3,40.6) vs (94.0,70.2,66.9); far middle (90.0,88.1,87.2) vs
  (111.1,103.8,102.1).
- after: canopy (105.6,98.0,93.1) vs (105.2,97.5,92.7); trunk
  (86.2,64.0,59.2) vs (88.2,64.5,60.4); far middle (84.8,81.3,72.6) vs
  (85.3,80.9,72.3); sky +2.4, ground +1.7, UI 0.0.

Remaining gaps:
- The simple path still handles only directional lights (point/spot are
  skipped) and has no specular vertex term; both exist in the mega VP.
- Simple-path TexEnv has 4 stages and no EMBM (mega has 8 stages + EMBM).
- The light table is re-uploaded (12 KB) per mega draw; it could be cached per
  frame until the light state changes.

## Integration with the texture-upload work (branch `vulkan-color-fog`)

Rebased onto `main/atys-live-n-vulkan` `adb36ad9d` (which contains
`0d6b19741` "upload textures with the frame, no per-update device wait",
`a463c5e44` merge + handoff commits). Conflicts were limited to
`driver_vulkan.cpp` (constructor init list: kept the new staging-ring fields,
dropped the old light-table buffers), `driver_vulkan_texture.cpp` (the
ForceDXTC early-out in `uploadTextureRegion` now sits in front of the new
`STextureCopy`/`recordTextureCopy` path; the BC3 upload writes through
`allocTextureStaging()`/`staging.Mapped`), and `AGENTS_todo.md` (kept both
handoff sections). The new upload architecture is untouched: per-frame-slot
upload command buffers submitted with the frame, host-visible staging ring,
`beginTextureCopy`/`endTextureCopy` gone, no per-update device waits. The
light table still goes through the per-frame uniform ring, and the simple
lighting/TexEnv changes are unchanged.

Verified in game on `mats005` (character `Kykikos`), 1280x800, post-rebase:

* Vulkan vs GL3, clean back-to-back captures (no time pin, ~3 min apart, so
  both drivers see the same client time): canopy +0.4/+0.2/+0.3, trunk
  +0.4/+0.3/-0.0, far left -0.9/-0.6/-0.6, far middle +0.7/+0.7/+0.8, ground
  -0.6/-0.5/-0.6, missions panel +0.1/-0.6/-0.0 (R,G,B, Vulkan minus GL3).
  Evidence `/tmp/opencode/fog/out/{glclean.png, vkclean-1800.png}`.
* Mega vs simple, both with the gdb-pinned hour (`set var
  ForcedDayNightCycleHour=12` at the `main` breakpoint; more reliable than
  typing into the chat, which can open UI windows): all scene regions within
  +/-0.6, ground 0.0, missions panel 0.0. Evidence
  `/tmp/opencode/fog/out/{vkgdb3-1800.png, vkmg-1800.png}`.
* ForceDXTC: `client_default.cfg` defaults `ForceDXTC = 1`, so the normal runs
  already exercise the path. Post-rebase validation run: **18** RGBA textures
  compressed to BC3, `NLVK_VALIDATION=1` reports only the two known gamescope
  VUIDs. No texture corruption; the earlier rainbow-looking frame was a game
  debug key toggled by typing into the world, not the upload path.
* `NLVK_STATS` in game (no validation, ForceDXTC on, ~805 indexed draws/frame):
  12 windows, **17.39..24.27 ms/frame, mean 19.2 ms/frame (52 fps mean)**,
  3.7 us descriptor+draw, 0.2 us index copy, 1323..4881 texture copies per
  120-frame window (batched, no upload submits), 0.01..0.03 MiB staged/frame.
  The absolute frame time is below the `vulkan-texperf` 29-30 ms measurement
  because that run had ~1200 draws/frame and another client on the account;
  the CPU cost per draw and the batched/no-wait upload path match it.

Pinning note for future agents: `/setLightHour 12` via xdotool is fragile
because the media-player dialog and the saved UI layout decide where clicks
land. Running the client under gdb and setting `ForcedDayNightCycleHour = 12`
at the `main` breakpoint is deterministic.

## Descriptor sets, CPU/GPU balance and the staging ring (branch `vulkan-descriptors`)

This section supersedes "Remaining hot spots" items 1 and 2 above: the
per-draw descriptor allocation is **not** 3.0 us/draw, and the frame is **not**
GPU bound in the Debug build.

### Descriptor sets: implemented, measured, reverted

A frame-slot cache was implemented and tested in two forms: descriptor sets
keyed on the four simple stage textures / eight mega textures plus the user
program block layouts, with dynamic UBO offsets (camera, object, material and
the three user blocks) and a hash map per frame slot; then the same with a
one-entry "same key as the previous draw" fast path. Both are frame-safe (the
cache is dropped with its descriptor pool after the slot's fence, and a uniform
ring growth clears it), and both rendered correctly. **Neither is faster than
the per-draw path on RADV, so the commit does not keep the cache.** The numbers
below are from the moor spot, 1280x800, Debug, `NLVK_STATS=1`, same binary for
the cache/no-cache rows (`NLVK_NODESCCACHE=1` was a temporary switch, removed):

| build | ms/frame | draws/frame | descriptor+draw |
|---|---|---|---|
| `a463c5e44` (baseline) | 30.8-33.3 | ~1205 | 3.0-3.2 us |
| hash cache + dynamic offsets | 31.3-32.7 | ~1184 | 3.4-3.5 us |
| same binary, cache off | 29.1-30.9 | ~1179 | 3.3-3.4 us |

With the cache, the hash lookup alone cost **0.69 us/draw**; with the cache off,
`vkAllocateDescriptorSets` + `vkUpdateDescriptorSets` of the full set (15 writes
on the mega path) cost **0.52-0.53 us/draw**. The lookup key is what makes the
cache lose: the mega key hashes eight image views and eight samplers. The
one-entry fast path brought the average back to 0.52-0.54 us (41% of lookups
were the same key as the previous draw), i.e. a wash, not a win.

What the 3.0 us actually contains (cache off, final build):

| per draw | us |
|---|---|
| pipeline-key build | 0.39-0.40 |
| `getPipeline` (`std::map` lookup) | 0.34-0.35 |
| bind: UBO staging + descriptor write + `vkCmdBind*` | 1.85-1.88 |
| vertex/index binds + `vkCmdDrawIndexed` | 0.47-0.48 |
| index copy (`getIndexUploadBuffer`) | 0.20 |

`NLVK_NOCULL` was scanned with `getenv()` on every draw; it is now read once.

### CPU vs GPU (the frame is CPU bound)

`NLVK_STATS=1` now writes two GPU timestamps around the frame command buffer
(per frame slot, read after the slot's fence) and also reports the CPU record
time from `beginFrame()` to `swapBuffers()`, plus the fence and acquire waits:

| metric | value |
|---|---|
| GPU time | **3.7-4.3 ms** |
| CPU frame record | **24.4-26.5 ms** |
| fence wait at `beginFrame` | 0.01 ms |
| acquire wait | 0.05 ms |
| wall frame | 29-31 ms |

The GPU is idle for ~85% of the frame. The earlier "CPU ~5 ms" was the driver's
draw submission (1170 x 3.1 us + index copies), not the engine: ~20 ms of the
frame is engine work outside the driver. This is a **Debug** build, so a
release build can shift the balance back; re-measure before deciding that
engine CPU is the target. Until then, further driver-side per-draw
micro-optimisation (including descriptor caching) has very little headroom.

### Texture staging ring: peak verified, memory returned

- Loading peaks at a **64 MiB ring** (32 MiB per frame in flight) with a
  **31.7 MiB frame peak** (seen on the `peak ... MiB/frame` column and from the
  grow messages, which now report the whole frame's requirement: 5.2, 8.7,
  17.0 MiB ...). In game the traffic is **0.01-0.03 MiB/frame** (20-60 copies
  per frame: font glyphs and remote-player textures).
- `growStagingRing()` now receives the whole frame's need instead of one copy's
  size, and `allocTextureStaging()` uses the current slot's base for the fit
  test; before, the first upload after a frame-slot switch always grew the ring
  (it compared against the previous frame's base).
- The ring shrinks back to **8 MiB** after 1800 consecutive frames below
  2 MiB/frame. In the final run the 64 MiB ring was built at 16:00:37 and
  shrunk at 16:01:52 (75 s; a texture burst resets the counter). The old buffer
  is retired with the frames that still read it, so there is no stall.
- Measured with `NLVK_STATS`: `ring 8192 KiB` and `peak 0.01-0.04 MiB/frame`
  once in game.
- The same loading frame also grows the **other** rings and they never shrink:
  vertex ring 128 MiB, uniform ring 64 MiB, index ring 32 MiB (in-game use is a
  few MiB per frame at most). That is ~224 MiB of host-visible memory left over
  from loading - the same idle-shrink pattern would apply, but it was left for
  the memory work, not done here.

### Audit (do not duplicate)

- **Depth buffer**: fixed on main by `fb6e5b5e5`; `createDepthResources()`
  allocates one depth image/view per swapchain image and `createFramebuffers()`
  pairs `_DepthViews[i]` with `_SwapchainViews[i]`. Confirmed in this tree.
- **ForceDXTC and fog/colour parity**: left to `vulkan-color-fog`.
- **Texture eviction**: still in place from `596557f25` (`_TexDrvShares`,
  `releaseTextureDrvInfo`).
- FullRewrite two-copy rotation, the character-selection black frame: unchanged,
  still open.

### Verification

- Build: `ninja -C build-vulkan -j 4 ryzom_client_dev lib/libnel_drv_vulkan.so`
  clean (the usual warnings only).
- In game with `NLVK_VALIDATION=1 NLVK_STATS=1`: no VUIDs apart from the known
  gamescope `VkSwapchainPresentModeInfoKHR` / `VK_EXT_surface_maintenance1`
  ones and the benign "vertex attribute not consumed" warnings. HUD, avatar and
  scene correct; clean `Quit -> Quit Now` logout; no `*.dmp`.
- Final non-validation run: 30.3-31.6 ms/frame (31.7-33.0 fps), desktop `u`
  window reads **FPS: 31**; screenshot
  `/tmp/opencode/desc/out/perf-u.png`, loading ring log in
  `/home/nuno/Projects/ryzom/ryzom_live_vk_desc/client.log`.

## Static build with the Vulkan driver linked in (`vulkan-static-build`) — done

Commit `a5762bcdc` on branch `vulkan-static-build` (based on `fb2ce0563`).
Goal met: one `ryzom_client_dev` binary runs the login screen from a standard
install with **no `LD_LIBRARY_PATH` and no `libnel*.so`** next to it.

### Driver change

- `nel/src/3d/dru.cpp` declares `createVulkanDriverInstance()` under
  `NL_STATIC` (+`NL_VULKAN_AVAILABLE`), next to the GL3 entry point.
- `nel/src/3d/driver/vulkan/driver_vulkan.cpp` defines it in `namespace NL3D`
  for `NL_STATIC`, and keeps `NL3D_createIDriverInstance` /
  `NL3D_interfaceVersion` inside `#ifndef NL_STATIC` for the shared-driver
  build. Same shape as `driver_opengl3.cpp`.
- Static drivers exposed a real duplicate-symbol bug: GL, GL3 and Vulkan each
  define `NLMISC::getMouseButton`, `getKeyButton`, `getKeyFromKeycode` and
  `getKeyFromKeySym` in `unix_event_emitter.cpp`, not declared in any header
  and used only in their own file. Linking two backends collided
  (`multiple definition`). They are now `static` in all three copies (no
  behaviour change; `getKeyFromKeyEvent` already was). This is what makes any
  multi-driver static build link, not just Vulkan.

### Build (this machine)

Full command in `docs/vulkan-static-build.md`. In short: the working
`build-vulkan` options plus `-DWITH_STATIC=ON -DWITH_STATIC_DRIVERS=ON`, and
the *shared external* fallback:

    -DWITH_STATIC_LIBXML2=OFF -DWITH_STATIC_CURL=OFF -DWITH_STATIC_EXTERNAL=OFF

Static libxml2/curl **do not link on Ubuntu 26.04**: `libcurl.a`/`libcrypto.a`
need zstd, brotli, idn2, psl, nghttp2, jitterentropy and `libfreetype.a` needs
bz2, none of which the CMake files add. With `WITH_STATIC_EXTERNAL=ON` the
find modules also pick every `.a`, so it fails even if curl is off. The
fallback keeps `libnel*` and the drivers static, which is the point.
`ninja -C build-static ryzom_client` gives `build-static/bin/ryzom_client_dev`
(534 MB Debug). glslang and SPIRV-Tools are linked from their static archives.

`ldd` result: no `libnel*.so`, no `libnel_drv_vulkan.so`; only system
libraries (libX11, libGL, libvulkan, libopenal, libfreetype, ogg/vorbis,
libcurl/libssl/libxml2, lua5.3, libc/libstdc++ ...). `nm -C` shows
`T NL3D::createVulkanDriverInstance()`. `client.log` has **no** `Using the
library` line (that is the dynamic path) and logs
`Vulkan: using 'AMD Radeon 840M (RADV GFX1153)'`. The static client also logs
`checkDriverVersion: Can't check video driver version`, which is expected: it
looks for the driver `.so` to read its version; harmless.

### Runtime test (standard install layout, no LD_LIBRARY_PATH)

Run dir `~/Projects/ryzom/ryzom_live_static_test`: `data/` and `cfg/`
symlinked to `~/Projects/ryzom/ryzom_live`, `client_default.cfg` copied,
`client.cfg` with `Driver3D="Vulkan"` (both occurrences), `RootConfigFilename`
-> the local copy, `FullScreen=0`, 1280x800, `SoundOn=0`. No `examples/` dir;
the client only warns about it.

- gamescope 1280x800, driver-side capture:
  `/tmp/opencode/vkstatic/login_ui.png` = full login UI (Login/Password
  fields, CONNECT, resolution/LOD strips, version
  `DEV Omega / v26.11.422 #fb2ce0563`); the earlier background shot is
  `login.png`. Frame 420 shot retried clean (`f420.ppm`).
- Window close (`xdotool windowclose` on the gamescope nested display from
  `/proc/PID/environ`, not the desktop display): the **client exits 0**
  (`CLIENTEXIT=0`), log shows
  `Vulkan: window destroyed (present: out of date); rendering stopped` and the
  full release path, no `*.dmp`. gamescope itself exits 139 after its child
  dies; don't read that as a client crash.
- One-off, not reproduced: the first run (gamescope failed to bind the desktop
  X0 socket and ended up on display :2) dumped a minidump in
  `recordScreenshotCopy` -> `vkCmdCopyImageToBuffer`, with a garbage image
  handle (non-canonical value). Frames 150, 420 (retry) and 1200 are clean, and
  so are all later runs. If a screenshot ever crashes right after a window
  resize/reconfigure, look at `_SwapchainImages[_ImageIndex]` there first.

### User instructions (repeat to the user)

1. Take `build-static/bin/ryzom_client_dev` (or ask the next agent to rebuild
   it) and copy it into `~/Projects/ryzom/ryzom_live`.
2. In `~/Projects/ryzom/ryzom_live/client.cfg` set `Driver3D = "Vulkan";`
   (the file contains the key twice; the client takes the last one, so change
   both). Keep `RootConfigFilename` pointing at that install's
   `client_default.cfg`.
3. Run `./ryzom_client_dev --nopatch 1`. No `LD_LIBRARY_PATH`, no copy of
   `libnel*.so`/`libnel_drv_vulkan.so`. The install's `libopenal.so.1` is
   enough; everything else is a normal system library.
4. `Driver3D = "OpenGL3"` or unset still works in the same binary (all three
   drivers are linked in).

### Descriptor caching

**Retest descriptor caching after future performance work (user request).**
The per-draw descriptor set is not the current bottleneck (see the audit
above); revisit it once a release build or engine-side CPU work changes the
GPU/CPU balance.

## User test of the static binary (2026-11-11) and the next work

The static `ryzom_client_vulkan` works in the user's install with
`Driver3D="Vulkan"`. Feedback:
- **`Driver3D="Auto"` does not start** (Vulkan does).
- Lots of output on stdout.
- 1280x800, HIGH, same spot: OpenGL official ~100+ FPS, our Vulkan static debug
  build ~30+ FPS, CPU 100% in both.

Two agents are working on it:
- `vulkan-driver-select` (mats005): fix Auto (prefer Vulkan when available),
  keep OpenGL/OpenGL3 working, quieten stdout, and verify Auto/OpenGL3/Vulkan
  from the same binary.
- `vulkan-perf` (mats006): full Vulkan-vs-GL3 performance assessment (CPU/GPU,
  draws, state changes, uploads), asset-format contribution, Debug vs FINAL,
  and safe wins with before/after numbers.

**Merge rule (user):** only merge into `main/atys-live-n-vulkan` once
Auto/OpenGL/Vulkan have all been verified working at the same time from the
same binary. Auto may prefer Vulkan when available.

## Config tool — Vulkan option (in progress)

Agent `vulkan-config-tool` (worktree `f1fb99/vulkan-config-tool`, base
`f26c0167f`, no game account) is adding a Vulkan choice to
`ryzom_configuration_qt` so it writes `Driver3D = "Vulkan";` (and checking the
OpenGL3 option too), building with Qt5 and verifying the written config.

## Qt configuration tool: Vulkan and OpenGL3 options (branch `vulkan-config-tool`)

`ryzom_configuration_qt` (the Qt client config tool) only offered Auto, OpenGL
and a Direct3D radio button (disabled outside Windows) — it ignored the
`OpenGL3`, `OpenGLES` and `Vulkan` values that `client_cfg.cpp` accepts. It now
offers:

- Auto (default, still the fallback for missing/unknown values),
- OpenGL,
- OpenGL3,
- Vulkan,
- Direct3D (Windows only, as before).

Selecting a driver writes the string form (`OpenGL3`, `Vulkan`, ...) to
`client.cfg`; loading accepts both the string and the numeric form (`4`, `5`)
case-insensitively, matching `client_cfg.cpp`. The comment in
`ryzom/client/client_default.cfg` now lists all five numeric values. The
video-mode list reuses the OpenGL modes for OpenGL3/Vulkan (same as Auto).

Built with Qt5 in `build-config` (`WITH_QT5=ON`, `WITH_QT=OFF`, otherwise the
same flags as `build-vulkan`) and verified under Xvfb (`QT_QPA_PLATFORM=xcb`,
1280x800): the tool displays all five radio buttons and writes
`Driver3D = "Vulkan";`, `"OpenGL3"`, `"OpenGL"` and `"Auto"` correctly from
successive runs; a config with `"5"`/`"4"` shows Vulkan/OpenGL3 checked and is
normalised to the string form on save. Run against a temp directory copy of
`client.cfg`/`client_default.cfg`, not the user's install.

Packages needed to build the tool: `qtbase5-dev`, `libqt5opengl5-dev`,
`qttools5-dev`, `qttools5-dev-tools`.

**Left:** the tool still does not offer `OpenGLES` (mobile driver, value `3`);
its translations for the changed driver label were not regenerated; the tool
writes only the string form, so an unquoted numeric `Driver3D` in an existing
`client.cfg` is not converted.

## Auto driver selection and quiet stdout (2026-11-11, `vulkan-driver-select`)

Worktree `~/.local/share/opencode/worktree/f1fb99/vulkan-driver-select`, base
`d36eab23c`, branch `vulkan-driver-select` (not pushed, not merged). Account
`mats005`; clients were only used up to the login screen.

### Why Auto failed

`Driver3D = "Auto"` never considered Vulkan. In `ryzom/client/src/init.cpp`
it picked OpenGL3 on 64-bit builds, Direct3D on Windows and OpenGL otherwise.
There was also no fallback: `CDRU::createGl3Driver()` throws when the shared
driver library is missing, and a driver whose `setDisplay()` fails went
straight to `ExitClientError`. On a layout where Vulkan was the working driver
(missing/incompatible GL3 library), Auto exited instead of starting. The static
driver registry itself is fine: `build-static/bin/ryzom_client_dev` contains
`createGlDriverInstance`, `createGl3DriverInstance` and
`createVulkanDriverInstance`.

The exact user failure could not be reproduced with the old static binary:
with Auto it selected OpenGL3 and reached the login screen both directly on the
desktop and under gamescope. The structural selection/fallback bugs above are
what this change fixes.

### Changes

- `getAutoDriverCandidates()`: Vulkan (when `NL_VULKAN_AVAILABLE`), OpenGL3
  (64-bit), Direct3D (Windows), OpenGL. First candidate is tried first.
- `createDriverSafe()`: catches the shared-driver exceptions (missing or
  incompatible library), logs a visible warning and moves to the next
  candidate.
- If the created driver cannot create the display, the next candidate is tried
  before giving up (`delete Driver` only after the replacement worked; its
  `_WindowInit` is still false, so teardown is the partial-state path already
  used on failure).
- Selection is logged: `3D: display created with the <driver> driver`.
- `client.cpp`: `DEFAULT_SD` (the printf/stdout displayer) is now removed from
  the Debug and Info logs in every build, not only `#ifndef NL_DEBUG`. The
  Warning, Error and Assert logs keep it, and `initLog()` still attaches
  `client.log` to all five streams. Debug/info therefore go to `client.log`,
  warnings/errors stay visible on stdout.

### Verification (one binary each)

Static `build-static/bin/ryzom_client_dev` (Debug, 534 MB), gamescope 1280x800:

| Driver3D | Starts | Log line | Visual |
|---|---|---|---|
| Auto | yes | `3D: display created with the Vulkan driver` | `NLVK` capture `static-auto/login.png` |
| OpenGL3 | yes | `3D: display created with the OpenGL3 driver` | host capture `static-gl3/host.png` |
| Vulkan | yes | `3D: display created with the Vulkan driver` | `NLVK` capture `static-vk/login.ppm` |
| OpenGL (legacy GL2) | yes | `3D: display created with the OpenGL driver` | host capture `static-gl/host.png` |
| Auto + broken Vulkan ICD | yes | `WRN ... Vulkan: initVulkan failed` then `WRN ... the Vulkan driver could not create the display, using OpenGL3` and `INF ... display created with the OpenGL3 driver` | GL3 rendering already proven above |

Dynamic `build-vulkan/bin/ryzom_client_dev` (same four drivers, plus two
fallback runs): same selection lines. With `libnel_drv_vulkan.so` moved away,
Auto logs `Loading library libnel_drv_vulkan.so failed` and
`3D: cannot create the Vulkan driver: nel_drv_vulkan not found`, then selects
OpenGL3 and reaches the login screen.

Captures live in `/tmp/opencode/vkauto/` (`static-*`, `test-*`,
`dyn-auto-final`); they are test artifacts, not committed.

### stdout before/after

Old `ryzom_client_dev_vulkan` (`fb2ce0563`) with Vulkan on Xvfb, 15 s:
111 lines = 10 DBG + 35 INF + 64 WRN + 2 blank/control lines.

New dynamic build, same Vulkan/Xvfb test: 67 lines = 0 DBG + 0 INF + 65 WRN.
Representative lines that are now only in `client.log`:

    INF ... initLog : RYZOM VERSION: DEV Omega / ...
    DBG ... config_file.cpp ... CF: Adding config file '...'
    INF ... driver_vulkan.cpp ... Vulkan: using 'AMD Radeon 840M (RADV GFX1153)'
    INF ... driver_vulkan_texture.cpp ... Vulkan: compressed texture 1024x1024 ...
    DBG ... driver_vulkan.cpp ... Vulkan: frame 600, ... indexed draws ...
    INF ... init.cpp ... 3D: display created with the Vulkan driver

Warnings are still printed, for example `Vulkan: initVulkan failed`,
`PATH: File (...) not found` and the static-build
`checkDriverVersion: Can't check video driver version`. The static old/new
counts are 681 -> 90 lines (47 -> 0 DBG/INF); its 632 -> 64 warning difference
is install-layout noise (`CPath::addSearchBigFile` duplicate adds when the
binary sits next to `data/`), not something suppressed by this change.

### Notes

- `docs/vulkan-static-build.md` now documents that Auto prefers Vulkan.
- Windows keeps its old GL3/D3D/GL preference order after Vulkan.
- Remaining: stdout is still warning-noisy on installs with duplicate big-file
  paths; that is intentional (warnings must stay visible). The per-frame Vulkan
  stats are gone from the terminal and remain in `client.log`.
- Do not merge before the user verifies the static binary in the real install.

## Bug: windowed resize freezes (Vulkan) — in progress

User report: with `Driver3D="Vulkan"` and `FullScreen = 0`, resizing the game
window freezes the game with a black screen. Agent `vulkan-resize` (mats005,
worktree `f1fb99/vulkan-resize`, base `681e78f91`) is investigating the
swapchain-recreate path, zero-sized extents, `_FrameOpen` state and semaphores,
and will fix/verify it.

## Queued: CPU -> GPU offload audit (after the perf assessment is merged)

User request: search the code for work done on the CPU that should be done on
the GPU (old CPU paths from the game's era, newer offload techniques). Examples
to look for: software skinning, matrix/palette transforms, particle/vegetation
updates, texture conversion/DXT compression at load, per-frame vertex/index
generation, sorting, fog/lightmap computation, CPU-side mipmapping, etc.
Any change must keep the other drivers working (OpenGL/GL3), preferably by
putting the mechanism in shared engine code rather than the Vulkan driver
alone. Dispatch a dedicated subagent after `vulkan-perf` is merged; it must
report candidates with cost/benefit and implement the safe ones with numbers.

## Performance assessment, Vulkan vs GL3/GL2, Debug vs FINAL (branch `vulkan-perf`)

Goal: explain the user's 30 fps (Vulkan) vs 100+ fps (official OpenGL client)
at 1280x800 HIGH on the same spot, and remove the driver-side part of it.

### Verdict in one line

**The 3x was mostly the build type, not the driver.** The same Debug binary
runs the same spot at 30 fps on Vulkan and 37-42 fps on GL3, while the FINAL
build of the same code runs 80-100 fps on Vulkan and 85-88 fps on GL3. On top
of that, the Vulkan driver did ~20% redundant CPU work per frame (texture
share-name rebuilds and a 12 KiB light-table copy per mega draw); both are
fixed, and FINAL Vulkan is now **faster than GL3 and GL2 in the same tree**
(118 fps vs 87 vs 75 at the same spot, same lighting).

### Measurement setup

- Worktree `f1fb99/vulkan-perf` at `d36eab23c`, branch `vulkan-perf`.
- Accounts: `mats006` slot 1 (character **Danili**, the client's own login
  spot: forest edge, birch trees, grass, flowers), HIGH settings
  (`Shadows=1 FXAA=1 Bloom=1 SquareBloom=1 CloudQuality=320 ForceDXTC=1`),
  1280x800, `InterfaceScaleAuto=1`, sound off.
- Run dir `/tmp/opencode/ryzomperf/live`: own `client.cfg` and
  `client_default.cfg`, `data`/`examples` symlinked to the user's install
  (`~/Projects/ryzom/ryzom_live`), driver switched in `client.cfg`.
- `gamescope --backend headless -W 1280 -H 800 -w 1280 -h 800 -r 240` so the
  client never shows a window on the desktop and FIFO present is not capped at
  60 Hz. Everything runs through the same wrapper, so all numbers share the
  same WSI path.
- Drivers: `Driver3D="Vulkan"`, `"OpenGL3"` and `"OpenGl"` (the legacy GL2
  driver, closest to the official client) from the same binaries.
- Timings: `NLVK_STATS=1` (driver timestamps + CPU record + per-draw cost),
  a temporary env-gated `NLGL_STATS` frame-time print in the two GL drivers for
  the GL side (**removed again before the commit**, see the patch note at the
  end), and the in-game `u` window FPS counter as a cross-check.
- CPU profiles: `sudo perf record -F 199 --call-graph fp` + `perf report`
  (perf_event_paranoid=4 on this box, so `sudo` is required).
- Debug build = `build-vulkan` (`-DCMAKE_BUILD_TYPE=Debug`), FINAL build =
  `build-final` (`-DCMAKE_BUILD_TYPE=Release -DFINAL_VERSION=ON`), both with
  all three drivers as shared modules.
- Watch out: another agent's build (4x cc1plus) or the user's own client moves
  the wall clock by 10-20%. The A/B numbers below were taken back to back in
  the same session and lighting; the "night" and "sunset" rows are different
  in-game times of day (GPU time differs, CPU time does not).

### Results (frames per second, same spot, same binary)

| build | driver | ms/frame | fps | CPU record | GPU time |
|---|---|---|---|---|---|
| Debug | Vulkan, before | 32.4-33.3 | 30-31 | 27.6-27.9 ms | 6.5-6.7 ms |
| Debug | Vulkan, after | **22.6-23.0** | **43-44** | 19.0-19.2 ms | 6.7-7.1 ms |
| Debug | OpenGL3 | 23.8-27.3 | 37-42 | - | - |
| FINAL | Vulkan, before (sunset) | 12.3-12.6 | 79-81 | 10.4-10.7 ms | 6.7-6.8 ms |
| FINAL | Vulkan, after (sunset) | **9.86-9.98** | **100-101** | 7.9-8.0 ms | 6.4-6.5 ms |
| FINAL | Vulkan, before (night) | 10.45-10.72 | 93-96 | 8.9-9.2 ms | 4.7 ms |
| FINAL | Vulkan, after (night) | **8.35-8.52** | **117-120** | 6.9-7.0 ms | 4.6-4.7 ms |
| FINAL | OpenGL3 | 11.4-11.7 | 85-88 | - | - |
| FINAL | OpenGl (GL2) | 13.3-13.7 | 73-75 | - | - |
| FINAL | Vulkan `NLVK_MEGA=1`, after | 8.30-8.54 | 117-120 | 6.8-7.0 ms | 5.3-5.5 ms |

The fair before/after pairs are the two with the same time of day: **-21% frame
time in FINAL** (both sunset and night) and **-32% in Debug**. The GL drivers
were measured twice (Debug and FINAL) with identical scene and spot.

Per-draw driver cost (`NLVK_STATS` "descriptor+draw"): 3.7-3.8 us Debug and
2.7-3.0 us FINAL before; **1.6-1.7 us after**, of which 0.2 us is the index
copy. The `u` window cross-check: with the validation layer on (12.8 fps from
`NLVK_STATS`) the window reads **FPS: 12**; screenshot
`/tmp/opencode/ryzomperf/shot.2700.png`.

### Where the frame went (perf, self time by shared object)

Debug, Vulkan (33 ms/frame):

| DSO | self |
|---|---|
| libnel3d (engine 3D) | 33.0% |
| libnelmisc | 23.2% (mostly the texture share-name strings) |
| libnel_drv_vulkan | 14.9% |
| libnelgui | 7.8% |
| ryzom_client_dev | 7.0% |
| libvulkan_radeon (RADV) | 4.8% |
| libc / kernel | 5.1% |

`CDriverVulkan::getTextureDrvInfos` alone was **18.6% of the Debug frame**
(inclusive): every `updateMaterialState()` rebuilt the share name of every
texture stage (a lower-case string copy plus two allocations) and searched a
`std::map` under the driver lock for it, 1445 times per frame. That is
Vulkan-specific: GL3 keeps the last bound texture per stage and skips the
setup entirely.

Debug, OpenGL3 (25 ms/frame) for comparison: libnel3d 34.0%, **libgallium
(Mesa) 17.7%**, libnel_drv_opengl3 11.8%, libnelgui 9.5%, client 9.2%,
libnelmisc 7.5%. Note `libnelmisc` is 23.2% on Vulkan and 7.5% on GL3: the
difference is the share-name work.

FINAL, Vulkan (12.5 ms/frame), before the fixes: libnel3d 26.8%,
**libnel_drv_vulkan 18.5%** (of which `stageMegaLightTableUBO()` **8.4%**,
`bindMegaDescriptors()` 2.9%), libvulkan_radeon 11.4%, libnelmisc 10.7%,
libc 7.4%, libnelgui 7.2%, client 5.7%. After the fixes: driver **11.0%**,
RADV 14.3%, and no single driver symbol above 2%.

So in Debug the frame is engine-bound because the engine is Debug code; in
FINAL the GPU takes 4.6-6.8 ms (55-65% of the frame) and the CPU work is the
remaining ~3.5-5 ms, of which the driver was ~1.5 ms and is now ~0.6 ms.

### Asset format: not the cause

Inventory of the 235 packs the client loads (`/tmp/opencode/ryzomperf/
texture_inventory.py`, reads the BNP tables and the DDS/TGA headers):

| format | entries | MiB on disk |
|---|---|---|
| DXT5 / BC3 | 20781 | 3354.9 |
| DXT1 / BC1 | 7199 | 578.7 |
| DXT3 / BC2 | 7 | 0.3 |
| TGA 16bpp | 1906 | 95.0 |
| TGA 8bpp (paletted) | 891 | 16.4 |
| TGA 24bpp | 318 | 1.6 |
| TGA 32bpp | 132 | 61.7 |
| PNG | 311 | 88.5 |

27283 of the DDS files carry a full mip chain. 3342 entries are <=64 px on the
smaller axis, 12715 <=256 px, 15177 >256 px. In game the client holds
**329 MB** of textures, uploaded as 1681 BC textures (log line "compressed
texture ... uploaded as BC"); `ForceDXTC=1` converts the RGBA ones to BC3 at
load. Per frame the client re-stages **0.00-0.01 MiB** in ~1680 small region
copies (font atlas glyph tiles and animated textures) - no per-frame
decompression, no per-frame format conversion.

Meshes: the driver never converts vertices. `getPipeline()` builds the
`VkVertexInputAttributeDescription` set once per vertex layout from the
`CVertexBuffer`'s own types and caches it; the layout is part of the pipeline
key. The layouts the spot actually draws (`NLVK_STATS` vertex-format
histogram, draws/frame): `0x407/36B` 538, `0xc1d/60B` 331, `0x7/32B` 126,
`0x40f/44B` 82, `0x7d/56B` 13, `0x1/12B` 10, `0x5/20B` 4, `0x3d/48B` 3,
`0x405/24B` 1 - i.e. interleaved float/normal/uv/packed-colour vertices of
12-60 bytes, the same bytes GL3 consumes.

Verdict: **the asset format is not a cause of the gap.** The data is already
in the Vulkan-friendly format (BC1/BC3 with mips). What a hypothetical new
format would buy: BC for the remaining ~3900 TGA/PNG entries (mostly small UI
and sprite textures, 263 MiB on disk, 4x VRAM if left RGBA) and BC5 for normal
maps. Neither shows up in the frame profile; the only measurable asset cost is
loading time, and that is already BC.

### What was changed (all in `nel/src/3d/driver/vulkan/`)

1. **Texture infos fast path** (`getTextureDrvInfos`): an untouched texture
   already has its `CTextureDrvInfosVulkan` in `TextureDrvShare->DrvTexture`;
   return it instead of rebuilding the share name and searching the map.
   Anything that regenerates a texture sets the touched flag, which keeps the
   old path. Measured: 1670-1730 of the ~1680 per-frame resolutions skip the
   lookup (NLVK_STATS "texture infos" line), 2-4 lookups per frame remain.
   Debug: getTextureDrvInfos 18.6% -> ~0.2% of the frame.
2. **Per-stage texture bind cache** (`updateMaterialState`): when the same
   `ITexture` is already bound to that stage and is untouched, skip
   `setupTextureEx()` completely. Invalidated by a `_TextureGeneration`
   counter bumped when a texture drv infos dies, so a recycled `ITexture`
   address can never hit a stale entry.
3. **Per-frame light table** (`bindMegaDescriptors`): the 128-entry (12 KiB)
   light-table block is uploaded once per frame (or after `setLight*()` /
   `setLightTableEntry()` / ring growth) instead of once per mega draw. This
   was 8.4% of the FINAL frame; with `NLVK_MEGA=1` it was ~12 KiB memcpy per
   draw.
4. **Pipeline-bind caching** (`bindPipeline`): `vkCmdBindPipeline` is skipped
   when the pipeline is already bound in this command buffer. Actual binds
   drop from 1.00 to **0.41-0.42 per draw** (the scene has ~6-70 pipelines).
5. **Blend-constant caching** (`setBlendConstants`): `vkCmdSetBlendConstants`
   only when the value changed (it was issued for every draw of both paths).
6. **`NLVK_STATS` grew the numbers the assessment needed**: draws, pipeline
   binds, descriptor sets and texture stage binds per frame, the
   vertex-format histogram (flags/stride x draws, most frequent first), and
   the texture-infos hit/lookup counters. Sample line:
   `Vulkan stats: state/frame: 1102 draws, 454 pipeline binds (0.41/draw),
   1147 descriptor sets (1.04/draw), 1439 texture stage binds (1.31/draw)`.

Unchanged: the simple path (default), the mega path's rendering, the GL2/GL3
drivers (the temporary `NLGL_STATS` patch used to measure them was reverted),
and the visual output (same textures, same UI, validation clean).

### Verification

- Builds: `build-vulkan` (Debug) and `build-final` (Release+FINAL) both link
  `ryzom_client`, `libnel_drv_vulkan.so`, `libnel_drv_opengl3.so`,
  `libnel_drv_opengl.so` cleanly.
- `NLVK_VALIDATION=1 NLVK_STATS=1` in game: only the two known gamescope WSI
  VUIDs and the benign "vertex attribute not consumed" warnings (same as
  before this branch).
- `NLVK_MEGA=1 NLVK_VALIDATION=1` in game: clean, renders correctly
  (`/tmp/opencode/ryzomperf/megashot.8000.png`).
- Screenshots after the fixes: `/tmp/opencode/ryzomperf/shot.1800.png`,
  `/tmp/opencode/ryzomperf/smoke.4500.png`, `/tmp/opencode/ryzomperf/
  finalshot.11400.png` (night and dawn lighting, HUD, avatar, landscape).
- Client shut down with SIGTERM; no `*.dmp` in the run dir.

### Remaining plan

- **Descriptor caching**: retest it now (the AGENTS_todo note asked for
  exactly this). At 1.04 sets/draw and 1.6 us/draw total the ceiling is
  ~0.3-0.5 us/draw; the prior attempt was a wash in a 33 ms Debug frame, it
  deserves one more look in a 8.5 ms FINAL frame where RADV is 14% of it.
  Cache key would be the 4/8 stage images + samplers with dynamic offsets.
- **The mega path is now the fastest configuration** (8.30 ms vs 8.35-8.52 ms
  and one less texture stage in some passes) and its per-draw UBO staging is
  still 4 blocks; if PPL/water get implemented there it should become the
  default instead of an opt-in.
- The remaining per-frame CPU is engine-side (`libnel3d` 27-33%,
  `libnelgui` 7-8%, `libnelmisc` 10-11% of FINAL). Landscape vegetable
  update, interface window updates and matrix math are the top symbols; none
  of that is driver-specific.
- GPU is now the majority of the frame at 120 fps (4.6 ms of 8.4 ms). The
  next wins there are bloom/FXAA/shadow passes (7.8 target binds/frame), not
  the main pass.
- To reproduce the GL-side numbers, the temporary patch was a 12-line
  `NLGL_STATS` block at the end of `CDriverGL3::swapBuffers()` and
  `CDriverGL::swapBuffers()` printing `NLMISC::CTime` deltas every 120
  frames; it is not in the commit.

## CPU -> GPU offload audit — in progress

Agent `vulkan-offload` (`mats006`, worktree `f1fb99/vulkan-offload`, base
`1dec94a8b`) is profiling the FINAL build's remaining engine CPU and moving the
safe, high-value work to the GPU, preferring shared engine code so OpenGL/GL3
keep working. Candidates: skinning/palette transforms, MRM geomorph, particle
and vegetation updates, per-frame vertex/index generation, matrix math,
texture conversion/mipmaps, sorting, lightmap/fog.

## Windowed resize — fixed (`vulkan-resize`, commit `d61e31182`)

Resizing the window in windowed mode (`FullScreen = 0`) froze the windowed
client on a black screen, and showed up as **two** bugs in the same path:

1. **Hang.** `swapBuffers()` handled the resize itself, after the frame had
   already acquired a swapchain image and reset that frame slot's fence. It
   destroyed the swapchain, discarded the frame (submit and present skipped)
   but left `_Fences[slot]` reset, then called `beginFrame()` for a new frame
   on the same slot: `vkWaitForFences(..., UINT64_MAX)` blocked forever.
   Reproduction at the login screen: resize once, process sleeps forever with
   `wchan = drm_syncobj_array_wait_timeout`; gdb stack
   `vkWaitForFences <- beginFrame (driver_vulkan.cpp:1503) <- swapBuffers
   (:1739) <- CDriverUser::swapBuffers <- loginMainLoop`.
   A second, spurious recreation was involved too: `_WindowWidth/_WindowHeight`
   were only refreshed by `isActive()`, which the game loop does not call, so
   after a present-driven recreate the cached size still held the old value
   and swapBuffers recreated the swapchain again in the next frame.

2. **In-game segfault.** On the first frame after a resize,
   `vkAcquireNextImageKHR` returns `VK_ERROR_OUT_OF_DATE_KHR`. `beginFrame()`
   recreated the swapchain and returned false. `beginDefaultRenderTarget()`
   (driver_user2.cpp) ignores `setRenderTarget()`'s return, so the scene was
   recorded with no target bound; `CFXAA::applyEffect()` then dereferenced
   `getRenderTarget()` (null) at `fxaa.cpp:351` -> SIGSEGV (core dump:
   `#0 NL3D::CFXAA::applyEffect` <- `mainLoop:1782`, `_RenderTargetTexture=0`,
   `_FrameOpen=true`). This only happens in game because that is where the
   FXAA/bloom default render target is bound.

### Fix (all in `nel/src/3d/driver/vulkan/driver_vulkan{.cpp,.h}`)

- The swapchain is replaced **at the start of `beginFrame()`**, before the
  fence wait and the acquire: no image is acquired and no command buffer is
  recording, so the old images can be destroyed and the fences keep their
  meaning. `swapBuffers()` only sets `_SwapchainOutOfDate` on present
  `OUT_OF_DATE`/`SUBOPTIMAL`; `setMode()` does the same (plus `XSync` so the
  surface sees the new geometry) instead of recreating synchronously.
- `beginFrame()` **retries the acquire once** after replacing an out-of-date
  swapchain instead of failing the frame, so `setRenderTarget()` cannot be
  left with "no frame" mid-frame. A failed acquire signals neither the
  semaphore nor the fence, so the retry reuses them safely.
- `createSwapchain()` clamps the extent to `[1, maxImageExtent]` (a minimized
  window reports 0x0, which `vkCreateSwapchainKHR` rejects) and caches the
  surface extent as the window size, so the next frame's size check and
  `getWindowSize()` agree with the images.
- `beginFrame()` skips frames while the window has no drawable size and asks X
  again (`isActive()`) before giving up, rather than creating a 0x0 swapchain
  or presenting into a surface nobody consumes.
- `recreateSwapchain()` refuses to run with a frame open (defensive warning)
  and marks the swapchain lost if the depth/framebuffer recreation fails,
  instead of continuing with null handles.

### Verification

Built Debug (`build-vulkan`), Xvfb 1600x1000 and gamescope 1280x800, windowed
1280x800, `NLVK_VALIDATION=1 NLVK_STATS=1`.

| run | what | result |
|---|---|---|
| login screen (gamescope + Xvfb) | 1024x640, 1400x900, 1x1, 1280x800, 800x600, 1600x1000 | one swapchain recreation per size, frames keep coming, `login-after-*` captures correct |
| in game (Xvfb, mats005) | 1280x800 -> 1024x640 -> 1400x900 -> 900x600 -> 1280x800, then 1x1, 640x400, 1600x1000, rapid 0.3 s alternation | no hang, no crash (`EXIT=0` at the end), swapchain recreated at every size, in-game capture `shots-game4/after-resizes.png` correct |
| in game (pre-fix, Xvfb) | 1024x640 -> 1400x900 | SIGSEGV in `CFXAA::applyEffect` (the bug above) |
| login screen (pre-fix, gamescope) | 1024x640 | hang in `vkWaitForFences`, gdb stack recorded |
| shutdown | `xdotool windowclose` (WM_DELETE) in game | exit 0, render targets released, no dump |
| validation | all of the above | only the known gamescope `VkSwapchainPresentModeInfoKHR` / `VK_EXT_surface_maintenance1` WSI messages and the pre-existing "vertex attribute not consumed" pipeline warnings |

Evidence in `/tmp/opencode/vkresize/`: `hang-first-resize.log` (pre-fix hang
log + gdb), core `2665770` in `coredumpctl` (FXAA crash), `run/client.log`,
`shots-game4/`, `shots-gs2/`, `shots/`, `xvfb-*.log`. Test scripts:
`run-gs.sh`, `run-xvfb.sh`, `resize-test.sh`, `resize-in-game.sh`,
`stop-gs.sh`. The run dir is `/tmp/opencode/vkresize/run` (data/cfg/examples
symlinked to `~/Projects/ryzom/ryzom_live`, `client.cfg` with
`Driver3D="Vulkan"`, `FullScreen=0`, 1280x800).

### Notes / left

- `setMode()` still only resizes the X window; there is no real fullscreen
  path in this driver (no override-redirect/XRandR mode switch). The deferred
  swapchain recreate is the same code path the resize tests exercise, and a
  `FullScreen = 1` start was checked (renders, exit 0). A true fullscreen
  toggle is still open, and it is a client/driver feature gap, not this bug.
- The engine ignores `setRenderTarget()`'s return value in
  `beginDefaultRenderTarget()`, so any future driver failure there crashes
  FXAA. The driver now avoids the failure, but the engine call site is worth
  hardening separately.
- `isActive()` still returns false for an unmapped window; the loading loop
  (`progress.cpp`) exits the client in that case. Pre-existing, untouched.

## Optimized static build with debug commands — in progress

Agent `vulkan-release-build` (mats005, worktree `f1fb99/vulkan-release-build`,
base `a85aff4b0`) is finding what actually gates Lua scripting and
`/setLightHour` (FINAL_VERSION vs WITH_RYZOM_LIVE vs build type), then building a
**static optimized** binary that keeps those commands, verifying Lua and
`/setLightHour`, and comparing fps against the Debug binary and the official
client. The optimized binary will be installed as
`~/Projects/ryzom/ryzom_live/ryzom_client_vulkan_opt`, keeping the Debug one.

## Optimized static client with Lua and /setLightHour (branch `vulkan-release-build`)

Worktree `f1fb99/vulkan-release-build` at `a85aff4b0`, branch
`vulkan-release-build`. Goal: the static client given to the user was
`CMAKE_BUILD_TYPE=Debug`, much slower than the official client, while still
needing the test commands. Built the same static configuration with
`Release` + `FINAL_VERSION=OFF`; it is 1.54x faster than the Debug static
client and on par with the official GL2 client at the same spot.

### What actually gates the features

| feature | gate | source |
|---|---|---|
| `/setLightHour` | `FINAL_VERSION=OFF` | `ryzom/client/src/commands.cpp:1471` opens `#if !FINAL_VERSION` for everything after the "COMMANDS after should NOT appear IN the FINAL VERSION" banner, closed at `:5185`. `NLMISC_COMMAND(setLightHour, ...)` is at `:5018`, inside it. The nested `#if FINAL_VERSION` privilege check at `:5028` is dead code. |
| `/luaScript`, `/luaInfo`, `/luaObject` | compiled in every build; runtime `AllowDebugLua = 1;` in `client.cfg` (default `false`, `client_cfg.cpp:714`) | `commands.cpp:5238`+ (after the `#endif` at `:5185`) |
| `/luaGC` | `FINAL_VERSION=OFF` | `commands.cpp:5333` |
| `WITH_RYZOM_LIVE` | **nothing**; only picks the ryzom.com URLs | `CMakeLists.txt:136` |

`FINAL_VERSION` is added as `-DFINAL_VERSION=1` when ON (`CMakeLists.txt:246`)
and defaults to `0` in `nel/include/nel/misc/types_nl.h:43`.
`CMAKE_BUILD_TYPE=Release` gives `-DNL_RELEASE -DNDEBUG -O3`
(`CMakeModules/nel.cmake:1083`); `NL_DEBUG`/`_DEBUG` are Debug-only. Neither
`NL_RELEASE` nor `NDEBUG` gates commands or Lua. The user brief's claim that
`-DWITH_RYZOM_LIVE=OFF` enables debug commands is wrong; it is only the URLs.

`FINAL_VERSION=OFF` does not cost measurable performance: `types_nl.h`
unconditionally defines `NL_NO_ASSERT`, so the `#if !FINAL_VERSION` asserts
are no-ops, and the other blocks are dev-only config reads and a few Lua
bindings (e.g. `openDoc`/`launchProgram`).

### Build

`CXXFLAGS="-D__STDCPP_DEFAULT_NEW_ALIGNMENT__=16"`, the options from
`docs/vulkan-static-build.md`, plus:

    -DCMAKE_BUILD_TYPE=Release -DFINAL_VERSION=OFF
    -DWITH_STATIC=ON -DWITH_STATIC_DRIVERS=ON
    -DWITH_STATIC_LIBXML2=OFF -DWITH_STATIC_CURL=OFF -DWITH_STATIC_EXTERNAL=OFF

`ninja -C build-static-release -j 8 ryzom_client` -> `bin/ryzom_client_dev`,
**36,620,952 bytes stripped** (Debug static is 533,980,792). `ldd` has no
`libnel*` and no `libnel_drv_*`; the Vulkan device is the static
`createVulkanDriverInstance()` (no "Using the library" line).

Installed as `~/Projects/ryzom/ryzom_live/ryzom_client_vulkan_opt` (md5
`43a0effe7b5257724ab291bcb8f3a47f`); the Debug binary
`ryzom_client_vulkan` is untouched. Run it from the install with the same
`Driver3D` config (`client.cfg`) and no `LD_LIBRARY_PATH`.

### In-game verification (mats005, slot 1, Kykikos, 1280x800, HIGH)

Headless gamescope 1280x800 @240 Hz, `NLVK_STATS=1`, run dir
`/tmp/opencode/relrun/live` (cfg + symlinked data; `AllowDebugLua = 1;`),
evidence in `/tmp/opencode/relrun/runs/`.

- `/setLightHour 12` typed in chat: scene goes from night to noon
  (`opt-vk/now.png`), `/setLightHour 22` gives dusk
  (`opt-vk/hour22.png`), and it sticks for the following minutes.
- `/luaScript messageBox("LUA_OK: optimized static luaScript works")` shows
  the box (`opt-vk/lua.png`); Lua errors from a deliberately wrong
  `displaySystemInfo` call come out in `client.log` as well.
- `ldd` on the installed copy: no NeL libraries.

### FPS at the same spot (all runs identical cfg/UI, light pinned to noon)

| build | driver | ms/frame (median) | fps |
|---|---|---|---|
| new static Release + FINAL_VERSION=OFF | Vulkan | 9.44 (9.10-13.60) | **106** |
| current static Debug `ryzom_client_vulkan` | Vulkan | 14.55 (12.88-17.32) | **69** |
| official `ryzom_client` (FV 4.1.0.0) | OpenGL (GL2) | u counter | **95-100** |

The official client cannot be pinned (`/setLightHour` is FINAL-only) and was
measured at its natural daylight, from the `u` configuration window
(`runs/official-gl2/u-window*.png`). The Vulkan numbers are medians of 25/30
`NLVK_STATS` 120-frame windows; the old pre-driver-fix Debug number (32.4 ms,
31 fps) is in the `vulkan-perf` section above and is not comparable with this
tree. The same Debug binary is faster now because it includes the per-draw
optimizations, which is why the remaining build-type gap is 1.54x, not 3x.

The static Release client was also run from its install path as
`ryzom_client_vulkan_opt`: it enters the game and its own frame log reads
600 frames per 4.0-5.0 s (~120-150 fps) in the current (dimmer) light.

### Left

- `AllowDebugLua = 1;` still has to be added to the user's `client.cfg` for
  `/luaScript` (setLightHour does not need it). It is not in the install's
  cfg yet; do not edit the user's install without asking.
- The binary is stripped (`WITH_SYMBOLS=OFF`); `-DWITH_SYMBOLS=ON` if a
  symbolized optimized build is wanted for crash dumps (larger).
- Nothing committed except the doc update; no client/gamescope left running.

## Crash: key-rebind save -> FD_SET overflow (in progress)

User report (optimized static, Release+FINAL_VERSION=OFF): press "k", rebind
movement to WASD, click Save -> freeze, then `You were disconnected by the
server` and `*** bit out of range 0 - FD_SETSIZE on fd_set ***: terminated`
(core dumped). Agent `vulkan-fdset-crash` (mats005, worktree
`f1fb99/vulkan-fdset-crash`, base `db380f605`) is reproducing it, checking
whether it is Vulkan-specific (also with OpenGL3), hunting the fd leak /
select() path and the save freeze, and fixing it.

## Key-rebinding Save crash: closed-socket FD_SET abort (branch `vulkan-fdset-crash`)

Worktree `f1fb99/vulkan-fdset-crash` at `db380f605`, branch
`vulkan-fdset-crash`. User report: in the optimized static client
(`ryzom_client_vulkan_opt`, Release + `FINAL_VERSION=OFF`), pressing `k`,
rebinding movement, then clicking **Save** froze for a moment and the client
aborted with `*** bit out of range 0 - FD_SETSIZE on fd_set ***: terminated`.

### It was not an fd leak

`coredumpctl` still had the user's core (PID 2730049, 2026-11-11 22:22:13,
`ryzom_client_vulkan_opt`). Its stack is:

    __libc_message -> __fortify_fail -> __GI___fdelt_chk
      <- CSock::dataAvailable()            (ryzom_client_vulkan_opt + 0xb01358)
      <- CUdpSimSock::dataAvailable()      (tail call to CSock::dataAvailable)
      <- client network update state       (ryzom_client_vulkan_opt + 0x5696a8)

At the abort, the `CSock` object (`0x5e2569501a40`, embedded as `UdpSock` in
`CUdpSimSock` at `CNetworkConnection::_Connection`) had `_Sock == -1`.
`glibc`'s `__fdelt_chk` rejects `-1` as well as `>= FD_SETSIZE`; the message
mentions FD_SETSIZE, but the descriptor was **INVALID_SOCKET**, not 1024+.
`client.log` confirms the sequence: `You were disconnected by the server` ->
`sendSystemDisconnection` -> abort. `CNetworkConnection::receiveNormalMessage`
handles `ACTION_DISCONNECTION_CODE` by calling `disconnect()`, which closes
`_Connection` (`_Sock = -1`); the same `stateConnected()` receive loop then
polls `_Connection.dataAvailable()` once more. `CSock::dataAvailable()` did
`FD_SET(-1)` and aborted.

No leak: `/proc/<pid>/fd | wc -l` was **413-416** in the user's static build
and **394-414** in the fixed Debug/Release runs, stable over 5+ minutes and
across repeated saves. Of those, ~252 are the two per-thread cached `FILE*`
handles of ~126 `.bnp` files (`CBigFile`), 60 sockets and 74 eventfds (Vulkan,
X11, sound); none grow per save or per frame. The fd never came near 1024.

The visible "freeze" is the save itself: `saveConfig()` visits every widget,
writes `save/interface_<char>.icfg` (~68 KB) and the FOW maps, then
`saveKeys()` writes `save/keys_<char>.xml` (~13 KB). In the user's log the
600-frame heartbeat took 7 s instead of 5 s, i.e. a ~2 s pause. It is not an
infinite loop and does not by itself crash the client; the abort is a separate
latent network bug that fires whenever the server disconnects the client.

### Fix

- `nel/src/net/sock.cpp`: `CSock::dataAvailable()` returns `false` for
  `INVALID_SOCKET`; on Unix it now waits with `poll()` instead of `select()`.
  `poll()` has no `FD_SETSIZE` limit and reports a stale descriptor as
  `POLLNVAL` instead of writing outside an `fd_set`.
- `nel/src/net/stdnet.h`: include `<poll.h>` on Unix.
- `CBufClient::sleepUntilDataAvailable`, `CBufServer::sleepUntilDataAvailable`
  and `CUnifiedNetwork::sleepUntilDataAvailable`: wait on their wake-up pipe
  with `poll()` (also Unix-only code). An invalid pipe returns instead of
  aborting.
- `CServerReceiveTask::run` / `CListenTask::run` (server-only, not built by the
  client) still use `select()`; they now check every socket/pipe descriptor
  before `FD_SET` and sleep/log once if one is invalid or `>= FD_SETSIZE`
  instead of aborting. A full `poll()` conversion of the server loops is left
  as a follow-up; the receive loop can legitimately have >1024 clients.

### Verification (1280x800 headless gamescope, account mats005 slot 1)

- Unit checks: a closed `CUdpSock::dataAvailable()` returns `0`; a valid UDP
  socket moved to fd **1100** (`> FD_SETSIZE`) also returns `0` instead of
  aborting.
- Exact crash path: a temporary `#ifdef NL_DEBUG` hook in `stateConnected()`
  closed `_Connection` right after a normal message (the same place the
  server-disconnection action does), then let the loop poll it again. With the
  fix the Debug client kept rendering frames 600-7800 for ~66 s and wrote no
  core; the hook was removed before committing.
- Debug + fixed static Release, Vulkan and OpenGL3: opened `k`, clicked
  **Save** five times in a row on each binary/driver. All stayed alive;
  static Vulkan fds 411-412, static GL3 fds 394, Debug Vulkan 400-411. No
  `FD_SET` abort, no new coredump. Key bindings persisted in
  `save/keys_kykikos.xml` (e.g. `KeyW -> backward`, `KeyD -> look_down`).
- Builds used for the test: Debug (`build-vulkan`) and a fresh static Release
  `FINAL_VERSION=OFF` tree (`build-static-release`, 35 MB, no `libnel*` in
  `ldd`), both from this branch.

### Left

- The ~2 s UI pause on Save is the normal full interface/FOW serialization;
  it was not optimized here. If it matters, `serialFOWMaps()` and the
  `CQuitVisitor` pass over all widgets are the places to look.
- The server receive/listen loops still use one `select()` each. Converting
  them to `poll()` would remove the >1024-client limit, but the client fix
  does not depend on it.
- No client/gamescope left running.

## CPU -> GPU offload audit (branch `vulkan-offload`, base `1dec94a8b`)

Task: find work still done on the CPU that should be on the GPU, profile it,
and move the safe/high-value parts. Account `mats006` slot 1, FINAL build,
1280x800, Vulkan (RADV 840M), same measurement wrapper as the perf assessment
(`gamescope --backend headless`, `NLVK_STATS=1`, `perf record -F 499 -g`).

### Verdict

The remaining FINAL CPU is engine logic (visibility, animation, UI layout,
matrix math); most of it has no honest GPU equivalent that fits a bounded
change. **The one significant GPU-offloadable block is software skinning +
MRM geomorph, and that path already exists in the tree** (the `glsl3vi` insert
vertex program, `gpu_skin_vp.cpp`). It works on GL3 but is disabled on Vulkan
because the insert path draws the character's rigid parts in the wrong place
(`supportVertexProgram(glsl3vi)` is opt-in behind `NLVK_GPUSKIN` because of
it). Fixing that is the next real offload; it is ~4% of the FINAL frame.

What was implemented here instead is the safe part: two measured CPU
reductions in shared engine code (they benefit every driver) plus a driver
capability split that separates "can do MRM geomorph" from "can do bone
skinning" and removes the engine's dependency on the single `glsl3vi` flag
(which is what made it impossible to enable one without the other).

### Profile ranking (FINAL, Vulkan, same spot; `perf`, self time)

DSO shares: `libnel3d` 33.7%, RADV 15.6%, `libnel_drv_vulkan` 12.0%,
`libnelmisc` 8.0%, `libnelgui` 7.9%, `ryzom_client` 5.8%, libc 3.8%,
kernel 3.7% (`vk-base` run; NLVK_STATS adds ~2.5% of `clock_gettime`).

Top self-time symbols (percent of process CPU):

| % | symbol |
|---|---|
| 2.73 | `NL3D::CLandscapeVegetableBlock::update` (per-block distance bucket) |
| 2.39 | `NLGUI::CInterfaceGroup::onFrameUpdateWindowPos` (zero-delta tree walk) |
| 2.21 | `NLVULKAN::CDriverVulkan::updateMaterialState` |
| 2.20 | `NLVULKAN::CDriverVulkan::bindMegaDescriptors` |
| 1.81 | `NL3D::CLandscape::render` (clear/preRender/refill/render) |
| 1.64 | `NLMISC::CMatrix::setMulMatrix` |
| 1.37+0.87+0.85+0.51 | `CMeshMRMSkinnedGeom::applyArrayRawSkinNormal2/3/4/1` (CPU skinning, 3.6% together) |
| 1.23 | `memmove` (uniform/index staging) |
| 1.11 | `CMatrix::setMulMatrixNoProj` (bone matrices) |
| 1.06 | `commitPushConstants` |
| 0.98 | `stageMegaObjectUBO` |
| 0.93 | `drawIndexed` |
| 0.75 | `CTrackSampledQuatSmallHeader::evalTime` |
| 0.74 | `CInterfaceElement::onFrameUpdateWindowPos` (part of the same walk) |
| 0.73 | `CPatch::preRender` |
| 0.63 | `CVegetableManager::render` |
| 0.59 | `CTrackSampledQuatSmallHeader::eval` |

Grouped: skinning ~4.0%, UI per-frame window walk ~3.1%, matrix math ~3.2%,
landscape (patch refine/render) ~3.0%, animation tracks ~2.4%, driver
per-draw (bind/stage/push) ~7%, particles/ribbons ~0.6%.

### Candidate table

| candidate | where | measured cost | GPU/offload equivalent | risk | GL/GL3 impact |
|---|---|---|---|---|---|
| GPU bone skinning | `gpu_skin_vp.cpp`, `mesh*skinned*.cpp`, `skeleton_model.cpp` | 4.0% of CPU (all `applyArrayRawSkin*`) | vertex shader bone palette (`glsl3vi` insert VP) — **already implemented** | **high today**: Vulkan draws rigid parts displaced; must be fixed and validated | GL3 uses the same path (it compiles the insert VP); GL2 unaffected |
| MRM geomorph | `mesh_mrm.cpp` render path, same insert VP | <0.5% in the measured spots (opaque MRM goes through `beginMesh`/VB heap, which has no GPU path) | same vertex program with `useSkeleton=0` | medium; only reached for non-mesh-block draws | shared |
| Landscape vegetable bucket update | `landscape_vegetable_block.cpp`, `landscape.cpp` | 2.7-4.8% | none (per-block visibility/LOD bookkeeping) | low → **implemented** as a CPU fast path | shared, all drivers |
| UI per-frame window walk | `interface_group.cpp`, `widget_manager.cpp` | 3.1% | none (CPU layout) | low → **implemented** as a zero-delta short-circuit | shared, all drivers |
| Per-frame bone matrices | `bone.cpp` (`setMulMatrixNoProj`) | 1.1% | compute shader skeleton update (big redesign) | high | shared |
| Matrix concatenation | `matrix.cpp` (`setMulMatrix`) | 1.6% | instance data in UBOs; CPU scene graph still needed | high | shared |
| Scene culling (spheres/planes/clip) | `mesh*.cpp`, `tess_block`, `clip_trav` | ~2.5% | GPU culling + indirect draws (big redesign) | high | shared |
| Particle/ribbon geometry | `ps_ribbon*.cpp` | ~0.6% | GPU particles/geometry shader (big redesign) | high | shared |
| Descriptor sets | `driver_vulkan_mega_render.cpp` | 1.04 sets/draw | dynamic offsets + cache — **already tried and was a wash** (see the descriptor section above) | - | Vulkan only |
| Texture upload/DXT | driver | ~0.01 MiB/frame in game | already BC + mips; no per-frame conversion | - | - |

### GPU skinning: what was found (do not redo this)

Facts established in game (Vulkan FINAL, `NLVK_GPUSKIN=1`, screenshots under
`/tmp/opencode/offload/`):

* With GPU skinning enabled, the character's rigid parts (gloves, boots,
  head/hair) are displaced and rotated; the CPU-skinned body is correct.
* `NLVK_SKIN_IDENTITY` (insert shader skips skinning entirely) renders a
  correct bind pose with every part attached → the GPU vertex buffer, the
  palette attribute (`R8G8B8A8_USCALED`, offset 80, stride 84) and the weights
  are right.
* The bone block staged into the Vulkan uniform ring matches the engine's
  `CUniformBuffer` byte-for-byte, and the descriptor at binding 6 points at
  that offset/range (`SKINDBG` logs, now removed).
* GL3 (same engine code, same shader source, same bone block) renders the
  avatar correctly and logs `GL3: compiled insert vertex program`.
* A debug insert shader that shifts vertices by +2 m had **no visible effect
  on GL3**, i.e. GL3 may not be executing the insert VP for those draws even
  though it compiles it. Verify that before assuming GL3 is the reference:
  put the shift in `gpu_skin_vp.cpp` (`NLVK_SKIN_SHIFT` was the temporary
  switch) and look at the gloves.
* Conclusion: the engine data path is correct; the remaining bug is in the
  Vulkan insert program's use of the bone block (binding/shader interaction),
  not in `fillGPUSkinBoneUBO`, the vertex layout or the descriptor staging.

The capability split added here (`IDriver::supportGPUSkinning()`,
`InterfaceVersion` 0x75) lets a future fix turn Vulkan skinning on without
also turning on the geomorph path, and guards `CMeshMRMSkinnedGeom::render`'s
non-skeleton insert path (previously it did not check the driver at all and
would try to use `glsl3vi` on GL2).

### Implemented: zero-delta UI tree walk (`interface_group.cpp`)

`CWidgetManager` calls `onFrameUpdateWindowPos(0,0)` on every top-level group
that needs a per-frame position (all `CGroupInScene` windows: name labels,
bubbles, flying text — 639 of them in the measured scene). The base
`CInterfaceGroup::onFrameUpdateWindowPos` then recursed through the whole
element tree adding zero. Every scene group is a top-level window
(`addWindowToMasterGroup`), so the recursion was pure no-op work.

`onFrameUpdateWindowPos` now returns after moving itself when `dx==dy==0`.
Non-zero deltas still move all children. Scene groups still get their direct
call from the widget manager, compute their projected position and call the
base with the real delta.

Measured (`perf`, same spot, FINAL):
* before: `CInterfaceGroup::onFrameUpdateWindowPos` 2.39% +
  `CInterfaceElement::onFrameUpdateWindowPos` 0.74% of all samples;
* after: **0 samples in both**.

### Implemented: vegetable distance-bucket fast path

`CLandscape::refine` called `CLandscapeVegetableBlock::update` for every
vegetable block every frame. `update` only does work when the view center
crosses a distance bucket (`NL3D_VEGETABLE_BLOCK_ELTDIST` steps); otherwise it
recomputes the same bucket and returns. The block now caches the squared
distance range of its current bucket (`_SqrDistMin/_SqrDistMax`, updated with
`_CurDistType`) and the refine loop skips the call while the view center stays
inside it. Bucket boundaries are inclusive, exactly matching the old
incremental loop; the initial state (no instance groups) uses
`[Array[NUMDIST], FLT_MAX]`, so the first update still happens when the block
becomes visible.

Measured (`perf`, giant-forest spot with a heavy vegetable field):
* before: `CLandscapeVegetableBlock::update` 4.78% of samples;
* after: `update` 0.68% + `isInCurrentDistanceType` 1.3% (the check inlines
  into `CLandscape::refine` in the final version).

### Before / after (FINAL, Vulkan, same spot and draw count)

Same client build, `NLVK_STATS=1`, three consecutive logins, ~920-922
draws/frame. Wall time includes GPU (4.1-4.3 ms); the CPU record is the
engine+driver path.

| build | CPU record | wall ms/frame | fps |
|---|---|---|---|
| base `1dec94a8b` | 6.10-6.88 ms | 8.09-8.57 | 117-124 |
| + UI zero-delta skip | 5.81-6.02 ms | 7.64 | 131 |
| + vegetable fast path | 5.60-5.87 ms | 6.92-7.32 | 137-144 |

Run-to-run variance on this box is a few percent (other agents' builds), so
treat the perf percentages above as the precise numbers and the wall-clock
rows as the end-to-end effect. Total: about **0.8 ms of the 6.5 ms CPU record
removed (-12%)**, ~0.9-1.3 ms of wall time.

### Verification

* FINAL `build-final` and Debug `build-vulkan` both build the client and all
  three drivers.
* Vulkan FINAL: rendering unchanged (screenshots `ui-simple.png`, `veg.png`,
  `veg2.png` under `/tmp/opencode/offload/`; HUD, name labels, avatar,
  vegetables, landscape).
* GL3 (same FINAL binary, `Driver3D="OpenGL3"`, Xvfb): starts, renders the
  same scene, labels and HUD correct (`gl3-final.png`).
* `NLVK_VALIDATION=1`: only the known gamescope `VkSwapchainPresentModeInfoKHR`
  VUIDs and the existing `vkCreateDevice` extension warning; no new errors.
* Client stopped with SIGTERM; no clients left running.

### Recommended next steps

1. Fix the Vulkan insert skinning (the facts above narrow it to the bone block
   as seen by the shader). Then enable `supportGPUSkinning()` for Vulkan and
   reclaim ~4% CPU. Verify GL3's insert path actually executes first.
2. GPU culling / indirect draws would attack the ~2.5% clip/transform work and
   the per-draw `setupModelMatrix`; that is the next structural offload.
3. `CWidgetManager::getWindowUnder` (0.6%) runs hit-testing every frame even
   when the mouse has not moved; cache by pointer position + UI generation.
4. Descriptor caching stays rejected (measured a wash, see above).

## Bug: Karavan buildings invisible under Vulkan (in progress)

At mats006 slot 1's saved position, the Karavan faction buildings are visible
with the official client but under Vulkan only the yellow horizontal bars show;
other building types are fine. The console also prints missing
`ma_sky_dome_gradient_fair.tga` and `63_*.zonel` warnings (probably unrelated;
the official client uses the same data). Agent `vulkan-karavan` (mats006,
worktree `f1fb99/vulkan-karavan`, base `caa64f610`) is reproducing, identifying
the material/shader/texture/vertex-program path used by those buildings and
fixing it, with GL3/official comparisons.

## Game UI keys that obscure the screen (agent note)

`m` opens the map, `i` the inventory and `u` the configuration window; the same
key closes them. They cover the world and ruin comparison screenshots, so
agents must not leave them open (press the key again, and check the client
shows the world before capturing).

## Karavan tents invisible (Specular + cube map) — `vulkan-karavan`

_2026-11-12, branch `vulkan-karavan`, Debug build `build-vulkan`, account
`mats006` slot 1 (the tutorial Karavan camp on Silan)._

**Symptom.** At mats006 slot 1's saved view the three Karavan mission tents
were invisible: only their yellow horizontal trim bands were drawn, floating in
mid-air. Every other object in the view (ship wreck, trees, NPCs, terrain,
UI) was fine.

**Root cause.** The tents are `CMaterial::Specular` with:

- stage 0 `ge_mission_tente_karavan.tga` (DXT5 512x512): its **alpha channel
  is the reflection mask**, not coverage (60% of the texels have alpha 0; only
  the golden band is opaque);
- stage 1 a `CTextureCube` reflection map (`spec_caravan.tga`);
- `IDRV_MAT_ALPHA_TEST` with threshold 0.5.

The Vulkan *simple* shader applied the material's TexEnv chain to `Specular`
(tex0 * tex1, alpha = tex0.a * tex1.a * vColor.a), so the fabric's alpha became
0 and the alpha test discarded it. Only the gold band (opaque alpha) survived.
GL2's Specular setup forces the stage alpha to the material/primary colour, and
the GL3 mega pixel shader keeps `fragColor.a`, so both pass the test.

The **mega** path already had the correct Specular branch, but
`canUseMegaPath()` rejected any material with a cube texture because
`uploadTextureCube()` was a stub. So even `NLVK_MEGA=1` silently fell back to
the broken simple path — that is why the mega A/B did not hide/show the tents.

**How it was found.** A temporary `NLVK_TRACE_MAT` trace (removed before the
commit) of frame 1500 listed three draws (d=80..82) with `shader=Specular,
mflags=0x2214, at=1 ref=0.5`, stage0 `ge_mission_tente_karavan.tga`, stage1
`spec_caravan.tga` repeated six times (a cube). Extracting
`ge_mission_tente_karavan.dds` from `objects.bnp` and reading its alpha showed
the mask. `NLVK_TEX_FILTER=ge_mission_tente_karavan` (only the tents drawn)
reproduced the bars on a black background. The official GL2 client and our GL3
driver show the fabric, confirming a Vulkan-only simple-path bug.

**Fix (commit `vulkan: cube maps + Specular on the mega path`).**

- `driver_vulkan_texture.cpp`: `uploadTextureCubeVulkan()` uploads the six
  `CTextureCube` faces into one `VK_IMAGE_VIEW_TYPE_CUBE` image (RGBA8,
  top mip, the same per-face H/V orientation fixes as the GL3 upload), with a
  1x1 white cube fallback; `createTextureImage()` takes `layers`/`cube`;
  `STextureCopy` carries `BaseLayer`/`LayerCount` so the layout barriers cover
  all six faces. Non-square cube faces (engine-generated character spec maps,
  128x256) cannot be a Vulkan cube: they fall back to face 0 as a 2D texture,
  which keeps the material on the simple path.
- `driver_vulkan_mega_render.cpp`: `canUseMegaPath()` accepts a cube on
  stage 1 and sets `cube=true`; `fillMegaPipelineKey()` uses the mega path for
  cube-textured **and** `Specular` materials even without `NLVK_MEGA` (the
  simple shader implements neither), and `bindMegaDescriptors()` binds the
  white cube where a cube stage has no image.
- `driver_vulkan_render.cpp`: the simple path binds the white 2D texture for a
  cube stage it cannot sample (no samplerCube in the simple fragment shader).

**Verification.**

- Debug Vulkan, same spot: the three tents render with the green fabric and
  gold bands, matching the official client (`vk-cube1/frame.ppm.1500.png`,
  `vk-final/final.png`, official `official-gl2/last.png`, GL3 `gl3-fix3/last.png`).
- `NLVK_VALIDATION=1`: only the pre-existing `vkCreateDevice` extension warning
  and the gamescope `VkSwapchainPresentModeInfoKHR` VUIDs; no cube/sampler
  errors (the first version had `VUID-VkImageCreateInfo-flags-08865` on the
  non-square cubes and layer-transition errors, both fixed).
- The optimized static binary (pre-fix, `ryzom_live_static_test`) reproduces
  the bars-only symptom (`vk-static-before/before.png`); it needs a rebuild to
  pick up the fix.

**Left.** Rebuild the static/FINAL binary to ship the fix; cube maps are top
mip only and always RGBA8 (no BC, no mips); non-square cubes use face 0 as a 2D
sphere map.

## Zora black screen with Medium+ landscape: descriptor pool exhaustion

User report: in Zora, with the `u` -> Graphics -> Landscape preset at Medium,
Normal or High (not Low), turning right so the stables are behind the avatar
made the whole screen black, UI included (minimap, char window, shortcut
bar); turning back restored it. Account `mats006` slot 0, character Matssix.

**Root cause.** Not the camera or a black quad. The driver allocated one
descriptor set per draw from a **fixed pool of 4096 sets per frame slot**
(`driver_vulkan.cpp`). A dense frame exceeded that, `vkAllocateDescriptorSets`
returned `VK_ERROR_OUT_OF_POOL_MEMORY` (-1000069000), and both
`commitPushConstants()` and `bindMegaDescriptors()` returned false without a
warning, silently skipping every remaining draw of the frame. The draws that
fell off the end are the **interface** and the **final scene blit**
(`CDriverUser::endDefaultRenderTarget`), so the swapchain kept the black
`_ClearColor` written at `beginFrame` and the whole window, UI included, was
black. Landscape presets at Medium+ enable micro-vegetation and a longer view
distance and push the frame over 4096 draws at that spot; Low stays under.
The black is *in the rendered image* (the driver's `NLVK_SCREENSHOT` frames are
all-zero too), not a present/compositor artifact.

**Evidence.**

- Repro: optimized build `build-rel` (`RelWithDebInfo`, same code as the
  user's `ryzom_client_vulkan_opt` @24a078312), Zora, `Bloom=0, FXAA=1,
  Shadows=1, Vision=400, LandscapeTileNear=40, LandscapeThreshold=500,
  MicroVeget=1`, 1920x1080, `gamescope --backend headless`, camera spinning.
  Driver-side `NLVK_SCREENSHOT_EVERY=60` dumps: about 1-2% of frames were
  `P6 ... 0 0 0` (all zero). With `FXAA=0` (no effect render target): **0
  black frames in 3915**. With `FXAA=1`: black frames recur.
- Driver-side traces (temporary, removed): a black frame's
  `setRenderTarget()` sequence is identical to a normal frame's
  (`effect target, main, FXAA target, main`), the final `drawQuad()` is
  reached (`NLVKRAW ... passes=1`), but `noteMainSampleTexture()` never runs,
  i.e. `commitPushConstants()` returned false right before the draw.
  Logging the allocation result gave `NLVKPOOL ... res=-1000069000` for
  every skipped draw (35413 failures in 40 s at that spot).
- After the fix: `NLVK_BLACKCHECK` readback detected **0 black frames in
  2701** at the same spot; a clean `RelWithDebInfo` run with
  `NLVK_VALIDATION=1` and screenshots every 60 frames produced 31/31
  non-black frames with the UI, and the pool growth message
  (`Vulkan: descriptor pool for frame slot 1 grown to 16384 sets`).
  The 1920x1080 stable view renders the stable boy, the avatar and the full
  UI; GL3 from the same binary renders the same view (no regression).
- Validation on the fixed build: only the pre-existing gamescope
  `VkSwapchainPresentModeInfoKHR` VUIDs, the `vkCreateDevice` extension
  warning and the known "vertex attribute not consumed" pipeline warnings;
  no descriptor-pool errors.

**Fix (commit `vulkan: grow the per-frame descriptor pools instead of
dropping draws`).** `driver_vulkan.h/.cpp`: `_DescriptorPools` is now a
`std::vector<VkDescriptorPool>[2]`; `allocDescriptorSet()` allocates from the
last pool, and on `VK_ERROR_OUT_OF_POOL_MEMORY`/`VK_ERROR_FRAGMENTED_POOL`
adds a larger pool (`4096 << min(poolCount, 5)` sets) and retries. All pools
are `vkResetDescriptorPool()`-ed (not recreated) when their frame slot is
reused, and destroyed in shutdown. Pools are sized for the mega layout too
(8 samplers, 7 UBOs per set); the old 4-sampler-per-set sizing capped the
mega path at 2048 sets. `driver_vulkan_render.cpp` and
`driver_vulkan_mega_render.cpp` use the helper.

**Left.** The fix is driver-local and does not touch GL/GL3. Possible
follow-ups: make the growth message a one-shot info (it currently prints per
new pool, which is fine); no other fixed-size per-frame limits were found in
this path (the vertex/index/uniform rings already grow). The user's static
`ryzom_client_vulkan_opt` must be rebuilt to ship the fix.
