# Port Ryzom to Vulkan — handoff

_Last updated: 2026-11-10 evening (unattended session)_

## Status in one line

**A native Vulkan driver (`nel_drv_vulkan`) runs the live client and renders the
UI.** 3D world rendering, text and the GL3 shader path are the next steps.

## Branch

`main/atys-live-n-vulkan` = `main/atys-live` + the OpenGL3 renderer (from
`origin/atys-n-gl3`, fast-forward `88e72dc35`) + the Vulkan driver work.

The old GL2 driver and the GL3 driver are both kept and untouched.

## What the Vulkan driver does today

- `nel/src/3d/driver/vulkan/` implements `IDriver` natively on Vulkan:
  - X11 window + `VK_KHR_xlib_surface` + swapchain + depth buffer + 2 frames in
    flight, present via FIFO.
  - `Driver3D="Vulkan"` in `client.cfg` selects it (also `"5"`); `Auto` still
    picks OpenGL3.
  - Textures: CPU RGBA8 conversion, device-local images, staging uploads; same
    size → in-place pixel update (animated textures); destroyed objects are
    deferred to the next frame so recorded commands stay valid.
  - Vertex buffers: host-visible device memory, `IVBDrvInfos` with lock/unlock,
    `setLocation(AGPResident)`.
  - Index buffers: engine keeps indices client-side; they are copied into a
    16 MB host-visible ring per frame.
  - Pipeline cache keyed on (vertex layout, blend, depth, cull, topology);
    shader variants per vertex format (position always; colour and texcoord0
    optional) compiled from GLSL at runtime with glslang.
  - Y is handled with a negative-height viewport (GL-like bottom-up) and GL
    depth is converted to Vulkan's [0,1] in the vertex shader.
- Verified: client starts, logs in, reaches the login screen and the game
  (newbieland), UI renders (see the screenshots in this session), validation
  layers are clean of rendering errors (only a gamescope WSI `VkSwapchainPresentModeInfoKHR`
  warning, which comes from the gamescope layer, not from us).

## Environment check: official client OK (2026-11-10)

Ran the live client (`~/Projects/ryzom/ryzom_live`) inside gamescope at 1280x800.
Auto-login worked (`Megana` in newbieland), input and clean logout verified.

    cd ~/Projects/ryzom/ryzom_live
    nohup gamescope -W 1280 -H 800 -w 1280 -h 800 -r 60 --backend wayland -- \
        ./ryzom_client mats007 passDOSma slot 1 --nopatch 1 \
        > /tmp/opencode/ryzomtest/gamescope.log 2>&1 &

- Arg order is `login password shard_id slot`; a non-numeric shard id (`slot`)
  falls back to the default (`ryzom/client/src/client.cpp:234`).
- Inside gamescope the X display is `:1` (host Xwayland is `:0`).
- **Screenshots**: `spectacle`-style host captures are unreliable on this box
  (they can come back fully transparent). Use the driver's own dump:
  `NLVK_SCREENSHOT=/path/frame.ppm NLVK_SCREENSHOT_FRAME=600` copies the
  rendered image to host memory inside the frame and writes a PPM.
  `convert frame.ppm frame.png` to view.
- Startup modal "Media player requires sound to be enabled" (no audio device);
  click OK at ~(130,782) if an OpenGL client is used.
- Clean exit: `Escape` -> "Quit Ryzom" -> "Quit Now". The avatar stays online
  for 30 s server-side; the login server says "already in online state" until
  then.

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
    ninja -C build-vulkan -j 8 ryzom_client_dev lib/libnel_drv_vulkan.so

- **Every cmake invocation needs `CXXFLAGS=...`**: the tree rebuilds
  `CMAKE_CXX_FLAGS` from `$ENV{CXXFLAGS}` (`CMakeModules/nel.cmake:540`) and the
  local breakpad headers need `__STDCPP_DEFAULT_NEW_ALIGNMENT__`.
- **Lua 5.3 + luabind-lua53 are mandatory** for the live 2026 data: with the
  default Lua 5.1 the interface scripts fail to parse
  (`gamedev.bnp@player.lua: unexpected symbol near '>'`).
- Vulkan link: `glslang glslang-default-resource-limits SPIRV SPIRV-Tools-opt
  SPIRV-Tools pthread` (order matters with static libs).
- Installed for the port: `libvulkan-dev glslang-dev glslang-tools spirv-tools
  libshaderc-dev vulkan-validationlayers`.

## Run the built client

`~/Projects/ryzom/ryzom_live_vk2` is a run dir with `data/`, `cfg/`,
`examples/` symlinked to `~/Projects/ryzom/ryzom_live` and a local `client.cfg`
(`Driver3D="Vulkan"`, 1280x800 windowed). Copy `build-vulkan/bin/ryzom_client_dev`
there as `ryzom_client` and run inside gamescope with
`LD_LIBRARY_PATH=<repo>/build-vulkan/lib`.

Useful env vars for the Vulkan driver:
- `NLVK_VALIDATION=1` — enable the Khronos validation layer.
- `NLVK_SCREENSHOT=<path>` + `NLVK_SCREENSHOT_FRAME=<n>` — dump frame n as PPM.
- `NLVK_NOCULL=1`, `NLVK_FORCECLEAR=1`, `NLVK_TESTTRI=1` — debugging helpers.

## Known gaps / next steps

1. **Text and some UI art are missing** (the login screen shows coloured bars
   but no glyphs). Fonts use partial texture uploads (`uploadTexture` with a
   rect) — the current implementation re-uploads the whole texture; check the
   font texture path and `CTextureFont`.
2. **3D world rendering** uses one simple forward shader: no lighting, no fog,
   no texenv modes, no vertex programs (NelVP skinning), no multi-texture
   combination. The GL3 mega shaders (`megaVPGenerate`/`megaPPGenerate` and
   `convertNelvpToGLSL`) are the intended source for a correct implementation;
   they are pure GLSL text builders and are meant to be lifted into the Vulkan
   driver (compile with glslang, bind the four builtin UBOs at bindings 0-3).
3. Render targets (`setRenderTarget`, bloom, FXAA, water reflection) return
   false/are stubs, so post effects are disabled.
4. `getBuffer()` (in-game screenshots) is still a stub; the debug dump can be
   promoted to implement it.
5. Performance: index uploads and per-draw descriptor sets are simple; put the
   index ring and descriptor sets on a per-frame budget and cache pipelines by
   material once the correctness work is done.
6. `mapTextureStageToUV`, `setRenderTarget` viewport scaling, cubemaps and
   compressed (BC) textures are not done; DXT textures are decompressed on
   upload, which costs memory and time.

## Useful references

- Driver interface: `nel/include/nel/3d/driver.h` (interface version `0x74`).
- GL3 driver for reference: `nel/src/3d/driver/opengl3/`, in particular
  `driver_opengl3_program.cpp`, `driver_opengl3_pixel_mega_program.cpp`,
  `driver_opengl3_vertex_mega_program.cpp`, `driver_opengl3_nelvp.cpp`,
  `driver_opengl3_uniform_buffer.cpp` (UBO layout) and
  `driver_opengl3_render_state.cpp` (camera/object/material staging).
- Driver selection: `ryzom/client/src/init.cpp:1064` and
  `nel/src/3d/dru.cpp` (`CDRU::createVkDriver`).
