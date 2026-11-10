# Port Ryzom to Vulkan — handoff

_Last updated: 2026-11-10, vertex programs (branch `vulkan-nelvp`)_

## Status in one line

**`nel_drv_vulkan` runs the live client on Vulkan: the login screen (all text),
the in-game UI, the sky, the landscape and the vegetation render, validation-clean.**
User vertex programs (nelvp assembly and the engine's GLSL/UBO programs) compile
and draw; lighting, lightmaps and post effects are still partial, so the world is
flat and the player avatar is still black (CPU skinning path, no vertex program).

## Branch

`main/atys-live-n-vulkan` = `main/atys-live` + OpenGL3 renderer (fast-forward
from `origin/atys-n-gl3`, `88e72dc35`) + the Vulkan driver, pushed to `origin`.
The vertex-program work is on local branch `vulkan-nelvp` (not pushed), based on
`de3bc1253`.

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
   `Specular` and the TexEnv path, but not PPL, water and fog (fog is generated
   but disabled). Lighting on many meshes is still flat; this is the main
   remaining rendering work.
2. **Vertex programs.** Implemented on branch `vulkan-nelvp` (see the section
   below). Still missing: the `glsl3vi` insert profile (GPU skinning / MRM
   geomorph), so the engine stays on CPU skinning and the avatar renders black.
3. **Render targets.** `setRenderTarget`/`copyTargetToTexture` are stubs, so
   bloom, FXAA, water reflection and shadow maps are disabled.
4. **Textures are never evicted** from `_TexturesVulkan` (no `ITextureDrvInfos`
   hook): a per-session leak, and a re-used `ITexture` address could show a
   stale texture.
5. **Partial texture uploads.** `uploadTexture` re-uploads the whole texture;
   compressed (DXT) textures are decompressed on upload.
6. **`getZBuffer`/`getBufferPart`** are stubs; `getBuffer` does a swapchain
   readback.
7. **Performance details.** One descriptor set per draw from a per-frame pool,
   no persistent pipeline cache, rings are single-buffered (they grow instead
   of wrapping).
8. Cubemaps are uploaded as 2D textures; `getTotalVideoMemory` is 4 GiB.

## Environment / run

    cd ~/Projects/ryzom/ryzom_live_vk2
    cp <repo>/build-vulkan/bin/ryzom_client_dev ryzom_client
    gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 --backend wayland -- \
      env LD_LIBRARY_PATH=<repo>/build-vulkan/lib ./ryzom_client --nopatch 1

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
still in flight and crashed RADV).


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

1. Fog parity: the mega shaders implement fog but it is currently disabled
   (`stageMegaObjectUBO`) because the simple path has none. Port fog to the
   simple shader (or make the mega path the default) and re-enable
   `data.fogEnabled`; GL3's black-fog overrides for additive blends and
   LightMap passes > 0 are already in place.
2. Verify the new LightMap multi-pass staging in game on a static-lightmapped
   object (no LightMap material was in view at the test spot).
3. Vertex programs: DONE on `vulkan-nelvp` for nelvp + glsl330v. Next is the
   `glsl3vi` insert profile so GPU skinning and MRM geomorph can run, then the
   avatar's CPU-skinning material/lighting path.
4. Render targets + bloom/FXAA/water reflection.
5. Texture eviction via an `ITextureDrvInfos` implementation.
6. Partial texture uploads, BC textures, cubemaps.

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
