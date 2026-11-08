FILE(GLOB SRC *.cpp *.h)

IF(WIN32)
  ADD_EXECUTABLE(zviewer WIN32 ${SRC} zviewer.rc)
ELSE()
  ADD_EXECUTABLE(zviewer ${SRC})
ENDIF()

target_compile_definitions(zviewer PRIVATE NL_ZVIEWER_CFG="${NL_ETC_PREFIX}/")

# plain (not keyword) form: NL_ADD_STATIC_VID_DRIVERS below uses plain
# TARGET_LINK_LIBRARIES, and CMake forbids mixing the two per target. Same form the
# samples use (nel/samples/3d/water/CMakeLists.txt:5); zviewer is an executable, so
# nothing downstream consumes these properties.
TARGET_LINK_LIBRARIES(zviewer NeL::misc NeL::3d)
# Without this, CDRU::createGl3Driver() returns NULL in a static build (NL_STATIC in
# nel/src/3d/dru.cpp:145 requires the driver to be linked, not dlopen'ed), so the new
# Gl3 knob could never take effect. The macro links exactly the drivers WITH_DRIVER_*
# enabled, per platform, so it is a no-op when they are off.
NL_ADD_STATIC_VID_DRIVERS(zviewer)
NL_DEFAULT_PROPS(zviewer "NeL, Tools, 3D: Zone Viewer")
NL_ADD_RUNTIME_FLAGS(zviewer)

INSTALL(TARGETS zviewer RUNTIME DESTINATION ${NL_BIN_PREFIX} COMPONENT tools3d)
INSTALL(FILES zviewer.cfg DESTINATION ${NL_ETC_PREFIX} COMPONENT tools3d)

IF(WITH_UNIX_STRUCTURE)
  INSTALL(FILES readme.txt DESTINATION ${NL_SHARE_PREFIX}/zviewer COMPONENT tools3d)
ELSE()
  INSTALL(FILES readme.txt DESTINATION ${NL_SHARE_PREFIX} COMPONENT tools3d RENAME zviewer.txt)
ENDIF()
