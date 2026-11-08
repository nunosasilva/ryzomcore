FILE(GLOB SRC *.cpp *.h)

IF(WIN32)
  ADD_EXECUTABLE(zviewer WIN32 ${SRC} zviewer.rc)
ELSE()
  ADD_EXECUTABLE(zviewer ${SRC})
ENDIF()

target_compile_definitions(zviewer PRIVATE NL_ZVIEWER_CFG="${NL_ETC_PREFIX}/")

# plain (not keyword) form, same as the other tools here and the samples
# (nel/samples/3d/water/CMakeLists.txt:5). zviewer is an executable, so nothing downstream
# consumes these properties either way.
# No NL_ADD_STATIC_VID_DRIVERS(zviewer): nel/src/3d/CMakeLists.txt already applies that macro
# to the nel3d library, so the enabled driver libs propagate through NeL::3d to every consumer
# (verified: with the call removed from this file, build-nel/.../zviewer.dir/link.txt still
# lists both nel_drv_opengl and nel_drv_opengl3, and CDRU::createGl3Driver() still resolves).
TARGET_LINK_LIBRARIES(zviewer NeL::misc NeL::3d)
NL_DEFAULT_PROPS(zviewer "NeL, Tools, 3D: Zone Viewer")
NL_ADD_RUNTIME_FLAGS(zviewer)

INSTALL(TARGETS zviewer RUNTIME DESTINATION ${NL_BIN_PREFIX} COMPONENT tools3d)
INSTALL(FILES zviewer.cfg DESTINATION ${NL_ETC_PREFIX} COMPONENT tools3d)

IF(WITH_UNIX_STRUCTURE)
  INSTALL(FILES readme.txt DESTINATION ${NL_SHARE_PREFIX}/zviewer COMPONENT tools3d)
ELSE()
  INSTALL(FILES readme.txt DESTINATION ${NL_SHARE_PREFIX} COMPONENT tools3d RENAME zviewer.txt)
ENDIF()
