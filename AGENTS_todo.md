# Port Ryzom to Vulkan — handoff

_Last updated: 2026-11-10, end of the unattended session_

## Status in one line

**`nel_drv_vulkan` runs the live client on Vulkan: the login screen (all text),
the in-game UI, the sky and the landscape render, validation-clean.** Lighting,
lightmaps, vertex programs and post effects are still missing; the world
therefore looks flat and some meshes (vegetation) are black.

## Branch

`main/atys-live-n-vulkan` = `main/atys-live` + OpenGL3 renderer (fast-forward
from `origin/atys-n-gl3`, `88e72dc35`) + the Vulkan driver, pushed to `origin`.

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

1. **Lightmaps / shader types.** Only the TexEnv path exists. GL3's mega
   shaders implement `CMaterial::LightMap`, `Specular`, PPL, water and fog;
   porting them (`megaVPGenerate`/`megaPPGenerate` are pure GLSL text builders,
   `convertNelvpToGLSL` too) is the main remaining rendering work. This is why
   vegetation and lightmapped meshes look black.
2. **Vertex programs.** `supportVertexProgram()`/`compileVertexProgram()` return
   false, so NelVP skinning, GPU skinning and the wind-tree VP never run
   (characters/vegetation are wrong or missing).
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
  `NLVK_NOCULL=1`, `NLVK_FORCECLEAR=1`, `NLVK_TESTTRI=1`.
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
- In game with `NLVK_MEGA=1` (verified 2026-11-10 18:23, character `Megana`,
  screenshot `/tmp/opencode/ryzomtest/megaworld.png`): the mega path is active
  but the **world staging is wrong** — the ground renders magenta and the sky /
  vegetation black, while some tree trunks show their textures. So the mega
  path still needs the terrain/lightmap/fog staging finished before it can
  replace the simple path.
- Validation-clean for the mega path (the only warning remains gamescope's WSI
  present-mode one).

Known gaps in the mega path (from the subagent that ported it):
- `CMaterial::LightMap` materials still fall back to the simple path (lightmap
  multi-pass staging is not implemented; reference: `beginLightMapMultiPass`,
  `computeLightMapInfos`, `setupLightMapPass` in
  `driver_opengl3_material.cpp`). This is almost certainly part of the magenta
  ground.
- Cubemaps on stages other than 1 fall back; cubemap upload is not implemented.
- TexGen and EMBM are not staged (the VP generator supports the modes).
- User vertex programs (bindings 4–6) are reserved but `supportVertexProgram()`
  is still false.

Next step there: port the lightmap pass staging (passes = ceil(lightmaps /
(maxSamplers-1)), stage 0 = material texture, stages 1..N = lightmap textures
with their factors as TexEnv constants), and re-check the terrain with
`NLVK_MEGA=1` in game. The magenta ground suggests the terrain material's
texture stages/texenv are staged incorrectly too. **Done on branch
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

## Build

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
3. Vertex programs: reuse `convertNelvpToGLSL` and the `NlNelvpConstants` UBO;
   then GPU skinning and the wind-tree VP can compile.
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
