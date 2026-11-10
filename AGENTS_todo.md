# Port Ryzom to Vulkan — handoff

_Last updated: 2026-11-10, late evening (mega shader port session)_

## Status in one line

**`nel_drv_vulkan` runs the live client on Vulkan: the login screen (all text),
the in-game UI, the sky and the landscape render.** The GL3 mega shaders are
ported and can replace the simple shader with `NLVK_MEGA=1` (login screen
pixel-identical); they are off by default. Vertex programs, texgen, lightmap
passes and post effects are still missing; the world therefore looks flat and
some meshes (vegetation) are black.

## Branch

`main/atys-live-n-vulkan` = `main/atys-live` + OpenGL3 renderer (fast-forward
from `origin/atys-n-gl3`, `88e72dc35`) + the Vulkan driver.

The GL2 and GL3 drivers are kept and still work (`Driver3D="OpenGL3"` verified
after the Vulkan changes: the GL3 mega shaders initialise and the client runs).

## What works (verified with screenshots)

- `nel/src/3d/driver/vulkan/` implements `IDriver` natively on Vulkan.
- `Driver3D="Vulkan"` (or `"5"`) selects it; `Auto` still picks OpenGL3.
- X11 window + `VK_KHR_xlib_surface`, swapchain, depth buffer, 2 frames in
  flight, FIFO present; device picks a supported depth format.
- Runtime GLSL→SPIR-V compilation with glslang; pipeline cache keyed by vertex
  layout + render state; shader variants per vertex format (position, normal,
  colour, 4 texcoord sets, optional).
- Textures: CPU conversion to RGBA8, device-local images, staging uploads,
  in-place updates for animated textures, alpha-only textures (fonts) become a
  white coverage mask, deferred destruction of objects still referenced by
  recorded frames.
- Vertex buffers in host-visible memory through `IVBDrvInfos::lock/unlock`;
  engine index buffers are copied into a 16 MB ring per frame.
- TexEnv interpreter in the fragment shader (same packing as
  `CMaterial::CTexEnv`), per-stage UV routing from `CVertexBuffer::getUVRouting`
  and `mapTextureStageToUV`, simple directional/ambient lighting from the light
  table (`setLights`/`setLightTableEntry`).
- Y is handled with a negative-height viewport (GL-like bottom-up), GL depth is
  converted to Vulkan [0,1] in the vertex shader, front face is CCW.
- **Mega shaders** (`driver_vulkan_mega.cpp`, `driver_vulkan_mega.h`,
  `driver_vulkan_mega_render.cpp`), enabled with `NLVK_MEGA=1`:
  the GL3 `megaVPGenerate`/`megaPPGenerate` are ported (fully UBO-backed
  variant, explicit `layout(location)` varyings, `#version 450`, UBO/sampler
  bindings injected); descriptor layout 0-3 builtin UBOs, 4-6 user UBO
  placeholders, 8-15 samplers; camera/light-table/object/material data staged
  like `driver_opengl3_render_state.cpp` + `driver_opengl3_light.cpp`;
  per-frame light-table buffer, per-draw camera/object/material from the
  uniform ring; PPL and fog paths are generated and driven by the object UBO.
  With `NLVK_MEGA=1` on the login screen: 2392/2392 draws use the mega path and
  the frame is pixel-identical to the simple path (ImageMagick AE=0).
  Materials the mega path cannot represent yet (LightMap, cubemaps outside
  stage 1) fall back to the simple shader per draw.
- ~60 fps (vsync) in game on RADV, ~600 draw calls/frame.
- Validation layer is clean apart from a gamescope WSI
  `VkSwapchainPresentModeInfoKHR` warning that comes from the gamescope layer.

## Known gaps

1. **Shader types.** The mega path implements `Normal`, `UserColor` and
   `Specular` (with the cube variant). `CMaterial::LightMap` still uses the
   simple path because lightmap texture passes are not set up; `Water` and the
   deprecated shaders render as Normal. EMBM matrices are not staged.
   Remaining GL3 reference code: `driver_opengl3_material.cpp` (`setupLightMapPass`,
   `setupWaterPass`).
2. **Vertex programs.** `supportVertexProgram()`/`compileVertexProgram()` return
   false, so NelVP skinning, GPU skinning and the wind-tree VP never run. The
   engine falls back to its fixed path, but skinned characters and vegetation
   are wrong or missing. The mega descriptor set already reserves bindings 4-6
   (user VP/PP/skeleton) for this.
3. **Render targets.** `setRenderTarget`/`copyTargetToTexture` are stubs, so
   bloom, FXAA, water reflection and shadow maps are disabled.
4. **Partial texture uploads.** `uploadTexture` re-uploads the whole texture;
   the font path works but is wasteful.
5. **`getBuffer()`** (in-game screenshots) is still a stub; the debug dump
   (`NLVK_SCREENSHOT`) is the working path.
6. **Performance details.** One descriptor set per draw from a per-frame pool,
   no pipeline cache persistence, no vertex buffer ring. Fine at the current
   draw counts, not great.
7. `getTotalVideoMemory()` returns a hard-coded 4 GiB.
8. Cubemaps are uploaded as 2D textures (`CTextureCube` path not implemented),
   so the Specular cube variant is generated but cannot get a cube view yet.

## Environment check: official client OK (2026-11-10)

    cd ~/Projects/ryzom/ryzom_live
    nohup gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 --backend wayland -- \
        ./ryzom_client mats007 passDOSma slot 1 --nopatch 1 \
        > /tmp/opencode/ryzomtest/gamescope.log 2>&1 &

- Arg order is `login password shard_id slot`; a non-numeric shard id (`slot`)
  falls back to the default (`ryzom/client/src/client.cpp:234`).
- Inside gamescope the X display is `:1` (host Xwayland is `:0`).
- **Screenshots**: host captures are unreliable on this box (the KDE session can
  return a fully transparent image). Use the driver's own dump:
  `NLVK_SCREENSHOT=/path/frame.ppm NLVK_SCREENSHOT_FRAME=600` (or
  `NLVK_SCREENSHOT_SEQ=1` for one every 600 frames, `prefix.<frame>.ppm`).
- After `kill -9` the account stays "online" for the network timeout + 30 s; the
  login then fails with "already in online state". Wait ~2 minutes between runs,
  or quit through `Escape` -> "Quit Ryzom" -> "Quit Now"
  (`/tmp/opencode/ryzomtest/stop_client.sh`).
- Startup modal "Media player requires sound to be enabled" (no audio device).

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

- **Every cmake invocation needs `CXXFLAGS=...`**: the tree rebuilds
  `CMAKE_CXX_FLAGS` from `$ENV{CXXFLAGS}` (`CMakeModules/nel.cmake:540`) and the
  local breakpad headers need `__STDCPP_DEFAULT_NEW_ALIGNMENT__`.
- **Lua 5.3 + luabind-lua53 are mandatory** for the live 2026 data; Lua 5.1
  fails to parse the interface scripts (`player.lua: unexpected symbol near '>'`).
- Vulkan link line order matters with the static glslang libs:
  `glslang glslang-default-resource-limits SPIRV SPIRV-Tools-opt SPIRV-Tools pthread`.
- Packages: `libvulkan-dev glslang-dev glslang-tools spirv-tools libshaderc-dev
  vulkan-validationlayers`.

## Run the built client

`~/Projects/ryzom/ryzom_live_vk2` is a run dir: `data/`, `cfg/`, `examples/`
symlinked to `~/Projects/ryzom/ryzom_live`, local `client.cfg`
(`Driver3D="Vulkan"`, 1280x800 windowed), `save/`. Copy
`build-vulkan/bin/ryzom_client_dev` there as `ryzom_client` and run inside
gamescope with `LD_LIBRARY_PATH=<repo>/build-vulkan/lib`.
(`~/Projects/ryzom/ryzom_live_vk2_mega` is a second run dir with the same layout,
used to test the mega shader path without racing the other client tests.)

Driver debug env vars: `NLVK_VALIDATION=1`, `NLVK_SCREENSHOT[_SEQ]`,
`NLVK_NOCULL=1`, `NLVK_FORCECLEAR=1`, `NLVK_TESTTRI=1`.

## Next steps (in order of value)

1. User vertex programs: reuse `convertNelvpToGLSL` (`driver_opengl3_nelvp.cpp`)
   as the mega VP insert source and the `NlNelvpConstants` UBO; then GPU
   skinning and the wind tree VP can compile and `supportVertexProgram()` can
   return true. Use descriptor bindings 4-6 already reserved by the mega layout.
2. Lightmap passes: stage `CMaterial::_LightMaps` into the mega sampler stages
   and the per-pass material UBO slots (see `setupLightMapPass` in
   `driver_opengl3_material.cpp`), then enable mega for `CMaterial::LightMap`.
3. TexGen: the mega VP already implements object/eye linear, reflection and
   sphere map from `nlTexGenMode`; the Vulkan driver stages `-1` (disabled).
   Copy the mode selection from `driver_opengl3_material.cpp`.
4. Cubemaps: implement `CTextureCube` upload (cube image + cube view) so the
   Specular mega variant gets a real cube sampler.
5. Render targets + bloom/FXAA/water reflection (start with
   `setRenderTarget`, `getRenderTargetSize`, `copyTargetToTexture`).
6. `getBuffer()` for in-game screenshots (the `recordScreenshotCopy` code in
   `driver_vulkan.cpp` already does the hard part).
7. Partial texture uploads and compressed (BC) textures.

## Reference map

- Driver interface: `nel/include/nel/3d/driver.h` (interface version `0x74`).
- Driver selection: `ryzom/client/src/init.cpp:1064`, `nel/src/3d/dru.cpp`
  (`CDRU::createVkDriver`), `ryzom/client/src/client_cfg.cpp` (`"Vulkan"`/`"5"`).
- UBO binding indices used by GL3: 0 camera, 1 light table, 2 model,
  3 material, 4 user VP, 5 user PP, 6 skeleton.
- GL3 attribute locations: `driver_opengl3_vertex.cpp` `GLVertexAttribIndex`.
