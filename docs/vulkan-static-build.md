# Static client build (3D drivers linked into the executable)

A static NeL client links `libnel*` and the 3D drivers into the executable, so
it can be dropped into a normal Ryzom install that only has the game data and
the system libraries. The point is testing the Vulkan driver without building
`libnel_drv_vulkan.so` next to it and without `LD_LIBRARY_PATH`.

## Driver entry point

With `WITH_STATIC_DRIVERS=ON`, CMake defines `NL_STATIC` and builds each driver
as a static library (`NL_TARGET_DRIVER`). `CDRU::createVkDriver()` then calls
`createVulkanDriverInstance()` instead of loading
`NL3D_createIDriverInstance` through `CLibrary`; the symbol is defined in
`nel/src/3d/driver/vulkan/driver_vulkan.cpp`, next to the shared-driver factory,
the same way the GL3 driver provides `createGl3DriverInstance()`. GL/GL3 and
the shared-driver builds are unaffected.

## Configure and build

From the repository root (`CXXFLAGS` is needed by the bundled `glslang` code):

```sh
CXXFLAGS="-D__STDCPP_DEFAULT_NEW_ALIGNMENT__=16" \
cmake -S . -B build-static -G Ninja \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFINAL_VERSION=OFF \
  -DWITH_RYZOM_LIVE=OFF \
  -DWITH_RYZOM_CLIENT=ON \
  -DWITH_RYZOM_SERVER=OFF \
  -DWITH_RYZOM_TOOLS=OFF \
  -DWITH_NEL_TESTS=OFF \
  -DWITH_NEL_SAMPLES=OFF \
  -DWITH_QT=OFF -DWITH_QT5=OFF \
  -DWITH_DRIVER_OPENGL=ON \
  -DWITH_DRIVER_OPENGL3=ON \
  -DWITH_DRIVER_VULKAN=ON \
  -DWITH_LUA51=OFF \
  -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
  -DLUA_LIBRARY=/usr/lib/x86_64-linux-gnu/liblua5.3.so \
  "-DLUA_LIBRARIES=/usr/lib/x86_64-linux-gnu/liblua5.3.so;/usr/lib/x86_64-linux-gnu/libm.so" \
  -DLUABIND_INCLUDE_DIR=$HOME/Projects/ryzom/luabind-install/include \
  -DLUABIND_LIBRARY_RELEASE=$HOME/Projects/ryzom/luabind-install/lib/libluabind_lua53.a \
  -DBREAKPAD_INCLUDE_DIR=$HOME/Projects/ryzom/breakpad/src \
  -DBREAKPAD_CLIENT_LIB=$HOME/Projects/ryzom/breakpad/src/client/linux/libbreakpad_client.a \
  -DWITH_STATIC=ON \
  -DWITH_STATIC_DRIVERS=ON

ninja -C build-static ryzom_client
```

`FINAL_VERSION=OFF` keeps the debug client (target `ryzom_client`, output
`bin/ryzom_client_dev`), including the chat/Lua commands used for testing.
`WITH_STATIC=ON` makes `WITH_STATIC_LIBXML2`, `WITH_STATIC_CURL` and
`WITH_STATIC_EXTERNAL` default to ON. If the distribution has no static
libxml2/curl, or the static link fails in them, add:

```sh
  -DWITH_STATIC_LIBXML2=OFF -DWITH_STATIC_CURL=OFF -DWITH_STATIC_EXTERNAL=OFF
```

The executable still links the system libraries (X11, OpenGL loader, Vulkan
loader, OpenAL, libc/libstdc++); only the NeL libraries and the 3D drivers are
inside it. Check with:

```sh
ldd build-static/bin/ryzom_client_dev   # no libnel*.so, no libnel_drv_*.so
```

## Optimized build that keeps the debug commands

`CMAKE_BUILD_TYPE=Debug` compiles the engine at `-O0` and is the reason the
static test client is slower than the official one. `CMAKE_BUILD_TYPE=Release`
makes `CMakeModules/nel.cmake` set `-DNL_RELEASE -DNDEBUG -O3`, which is the
dominant speed difference; `FINAL_VERSION=OFF` costs nothing measurable
because `NL_NO_ASSERT` is defined for every build (see `nel/include/nel/misc/types_nl.h`), so the
`#if !FINAL_VERSION` blocks are mostly no-op asserts.

What actually gates the commands:

| feature | gate | where |
|---|---|---|
| `/setLightHour` | `FINAL_VERSION=OFF` — the command is inside `#if !FINAL_VERSION` | `ryzom/client/src/commands.cpp:1471` (`#if !FINAL_VERSION`) to `:5185`; `NLMISC_COMMAND(setLightHour, ...)` at `:5018` |
| `/luaScript`, `/luaInfo`, `/luaObject` | compiled in every build; enabled at runtime by `AllowDebugLua = 1;` in `client.cfg` (default false) | `commands.cpp`, `ryzom/client/src/client_cfg.cpp:714` |
| `/luaGC` | `FINAL_VERSION=OFF` | `commands.cpp:5333` |
| `WITH_RYZOM_LIVE` | nothing — it only selects the ryzom.com URLs | `CMakeLists.txt:136` |

So build the same static configuration with `-DCMAKE_BUILD_TYPE=Release` and
`-DFINAL_VERSION=OFF` (the rest unchanged, including the static external
fallback):

```sh
CXXFLAGS="-D__STDCPP_DEFAULT_NEW_ALIGNMENT__=16" \
cmake -S . -B build-static-release -G Ninja \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DFINAL_VERSION=OFF \
  -DWITH_RYZOM_LIVE=OFF \
  -DWITH_RYZOM_CLIENT=ON \
  -DWITH_RYZOM_SERVER=OFF \
  -DWITH_RYZOM_TOOLS=OFF \
  -DWITH_NEL_TESTS=OFF \
  -DWITH_NEL_SAMPLES=OFF \
  -DWITH_QT=OFF -DWITH_QT5=OFF \
  -DWITH_DRIVER_OPENGL=ON \
  -DWITH_DRIVER_OPENGL3=ON \
  -DWITH_DRIVER_VULKAN=ON \
  -DWITH_LUA51=OFF \
  -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
  "-DLUA_LIBRARIES=/usr/lib/x86_64-linux-gnu/liblua5.3.so;/usr/lib/x86_64-linux-gnu/libm.so" \
  -DLUABIND_INCLUDE_DIR=$HOME/Projects/ryzom/luabind-install/include \
  -DLUABIND_LIBRARY_RELEASE=$HOME/Projects/ryzom/luabind-install/lib/libluabind_lua53.a \
  -DBREAKPAD_INCLUDE_DIR=$HOME/Projects/ryzom/breakpad/src \
  -DBREAKPAD_CLIENT_LIB=$HOME/Projects/ryzom/breakpad/src/client/linux/libbreakpad_client.a \
  -DWITH_STATIC=ON \
  -DWITH_STATIC_DRIVERS=ON \
  -DWITH_STATIC_LIBXML2=OFF \
  -DWITH_STATIC_CURL=OFF \
  -DWITH_STATIC_EXTERNAL=OFF

ninja -C build-static-release ryzom_client
```

The output keeps the `_dev` name (`bin/ryzom_client_dev`, about 35 MB:
`WITH_SYMBOLS=OFF` strips it, add `-DWITH_SYMBOLS=ON` if crash dumps need
symbols). Copy it next to the debug build as `ryzom_client_vulkan_opt` so both
can be kept. `/luaScript` needs `AllowDebugLua = 1;` in `client.cfg`;
`/setLightHour 12` works as a normal chat command and a negative value resets
the automatic cycle.

## Tier 1 static third-party libraries (Linux)

The official client statically links most third-party libraries but keeps the
platform ABI libraries dynamic.  The closest low-risk subset is:

- **static**: lua, zlib, libpng, libjpeg, gif, libxml2 (+lzma), vorbis and
  vorbisfile;
- **dynamic**: curl/OpenSSL and freetype (Tier 2, security-sensitive),
  GL/GLX, X11/Xrandr/Xrender/Xxf86vm/Xcursor, OpenAL, the Vulkan loader,
  libc/libm/libstdc++.

No new CMake option is needed.  `WITH_STATIC_EXTERNAL=ON` makes
`find_library()` prefer `.a`; `WITH_LUA53=ON` lets the repo's
`FindLua53.cmake` find `liblua5.3.a` without hardcoding its path.  The global
suffix preference would also pick static curl/OpenSSL/freetype, so force those
few cache entries back to `.so`:

```sh
CXXFLAGS="-D__STDCPP_DEFAULT_NEW_ALIGNMENT__=16" \
cmake -S . -B build-static-tier1 -G Ninja \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release -DFINAL_VERSION=OFF \
  -DWITH_STATIC=ON -DWITH_STATIC_DRIVERS=ON \
  -DWITH_STATIC_EXTERNAL=ON -DWITH_STATIC_LIBXML2=ON \
  -DWITH_STATIC_CURL=OFF \
  -DWITH_LUA51=OFF -DWITH_LUA52=OFF -DWITH_LUA53=ON \
  -DLUA_MATH_LIBRARY=/usr/lib/x86_64-linux-gnu/libm.so \
  -DCURL_LIBRARY_RELEASE=/usr/lib/x86_64-linux-gnu/libcurl.so \
  -DFREETYPE_LIBRARY_RELEASE=/usr/lib/x86_64-linux-gnu/libfreetype.so \
  -DOPENSSL_SSL_LIBRARY=/usr/lib/x86_64-linux-gnu/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/x86_64-linux-gnu/libcrypto.so \
  -DLUABIND_INCLUDE_DIR=$HOME/Projects/ryzom/luabind-install/include \
  -DLUABIND_LIBRARY_RELEASE=$HOME/Projects/ryzom/luabind-install/lib/libluabind_lua53.a \
  -DBREAKPAD_INCLUDE_DIR=$HOME/Projects/ryzom/breakpad/src \
  -DBREAKPAD_CLIENT_LIB=$HOME/Projects/ryzom/breakpad/src/client/linux/libbreakpad_client.a \
  -DWITH_RYZOM_LIVE=OFF -DWITH_RYZOM_CLIENT=ON -DWITH_RYZOM_SERVER=OFF \
  -DWITH_DRIVER_OPENGL=ON -DWITH_DRIVER_OPENGL3=ON -DWITH_DRIVER_VULKAN=ON \
  -DWITH_NEL_TOOLS=OFF -DWITH_NEL_TESTS=OFF -DWITH_NEL_SAMPLES=OFF

ninja -C build-static-tier1 ryzom_client
```

`WITH_STATIC_CURL=OFF` is important when curl stays dynamic: otherwise the
build defines `CURL_STATICLIB` while linking a `.so`.  On a distribution where
`FindLua53` picks a static `libm.a`, point `LUA_MATH_LIBRARY` at `libm.so` (as
above) to avoid a static libm in the PIE.

Verify the result:

```sh
readelf -d build-static-tier1/bin/ryzom_client_dev | grep NEEDED
ldd build-static-tier1/bin/ryzom_client_dev | grep -E \
  'liblua|libxml2|libpng|libjpeg|libgif|libz|libvorbis' || true
```

The Tier 1 libraries must not appear in either list; curl, OpenSSL, freetype,
GL, X11, OpenAL and Vulkan should.  A test session with the static binary
renders the normal UI, textures and NPCs; `Media player requires sound` is the
usual modal, not a link failure.

## Run from a normal install

Copy `build-static/bin/ryzom_client_dev` into the install directory (for
example `~/Projects/ryzom/ryzom_live`). `Driver3D = "Auto"` now selects Vulkan
when it is compiled in and can create the display, and falls back to OpenGL3 or
OpenGL otherwise. To force one driver, set it in that directory's `client.cfg`:

```
Driver3D = "Vulkan";    // or "OpenGL3", "OpenGL"
```

`RootConfigFilename` in `client.cfg` must point at the `client_default.cfg` of
that install (it normally already does; a run directory with its own copy must
point at its local copy). No `LD_LIBRARY_PATH` is needed. The install's own
`libopenal.so.1` or the system `libopenal` provides sound.
