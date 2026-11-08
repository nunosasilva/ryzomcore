ADD_EXECUTABLE(nelvp_glsl_test main.cpp)

# driver_opengl3.h is a private header of the parent directory - this is the
# only consumer outside that directory, and it is a test, which is why the
# include dir is added explicitly instead of the header being made public.
TARGET_INCLUDE_DIRECTORIES(nelvp_glsl_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# plain (not keyword) form, like every other target here. ${NLDRV_OGL_LIB} is
# the parent scope's target name (nel_drv_opengl3, or _win on Windows), which
# is why this test lives in the driver's directory and not under tools/3d.
TARGET_LINK_LIBRARIES(nelvp_glsl_test ${NLDRV_OGL_LIB} NeL::3d NeL::misc)

NL_DEFAULT_PROPS(nelvp_glsl_test "NeL, 3D, Drivers: NeLVP to GLSL Converter Test")
NL_ADD_RUNTIME_FLAGS(nelvp_glsl_test)

# No INSTALL: a test, not a shipped tool.
ADD_TEST(NAME nel_gl3_nelvp_glsl_convert COMMAND $<TARGET_FILE:nelvp_glsl_test>)
SET_TESTS_PROPERTIES(nel_gl3_nelvp_glsl_convert PROPERTIES TIMEOUT 60)
