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
