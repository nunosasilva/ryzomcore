ADD_SUBDIRECTORY(clip_plane)
ADD_SUBDIRECTORY(navigation)
ADD_SUBDIRECTORY(nelvp)
ADD_SUBDIRECTORY(planar_reflection)
ADD_SUBDIRECTORY(ppl)
ADD_SUBDIRECTORY(water)
ADD_SUBDIRECTORY(font)
ADD_SUBDIRECTORY(font_perf)
ADD_SUBDIRECTORY(cluster_viewer)
ADD_SUBDIRECTORY(shape_viewer)
ADD_SUBDIRECTORY(landscape)
ADD_SUBDIRECTORY(decal)
ADD_SUBDIRECTORY(specular)

IF(WITH_NEL_CEGUI)
  ADD_SUBDIRECTORY(cegui)
ENDIF()

# ---------------------------------------------------------------------------
# Headless OpenGL3 smoke test (task 0.3)
#
# Runs a sample under Xvfb + the Mesa software renderer and fails if the sample
# dies early, if the driver logs an ERR/FATAL, or if the captured frame is blank
# (unique colour count, so "it ran but drew nothing" cannot pass).
#
# Only the samples that actually ask for UDriver::OpenGl3 are registered - the
# others (font, font_perf, navigation, cluster_viewer, shape_viewer) do not,
# so a pass there would not cover the GL3 driver at all.
# headless_smoke.sh exits 77 when Xvfb/xwininfo/import/identify are missing, which
# ctest reports as "Not Available" rather than a failure.
#
# MULTI-ARCH: this harness is Unix-only by design - Xvfb has no Windows or macOS
# equivalent. It is therefore NOT REGISTERED at all off Unix: the FIND_PROGRAM
# gate below is what keeps a Windows/Mac build from ever seeing these tests, and
# the script's own exit 77 is only the second line of defence. Do not "fix" the
# tests on Windows by faking a frame grab; GL3 coverage there is separate work
# (a real window + GPU, or a software GL). The sample *code* stays cross-platform;
# only this test harness is not.
# ---------------------------------------------------------------------------
IF(WITH_DRIVER_OPENGL3 AND (WITH_NEL_TESTS OR WITH_TESTING))
  FIND_PROGRAM(NL_HEADLESS_GL_TOOLS Xvfb)
  # bash is resolved explicitly rather than relying on the script's exec bit: a
  # Windows checkout or a zip export loses file modes, and ctest would then fail
  # with "Unable to execute" instead of skipping.
  FIND_PROGRAM(NL_BASH bash)
  IF(NL_HEADLESS_GL_TOOLS AND NL_BASH)
    FOREACH(NL_GL3_SMOKE_SAMPLE clip_plane nelvp planar_reflection ppl water landscape decal specular)
      IF(TARGET nl_sample_${NL_GL3_SMOKE_SAMPLE})
        ADD_TEST(NAME nel_gl3_smoke_${NL_GL3_SMOKE_SAMPLE}
                 COMMAND ${NL_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.sh
                         14 $<TARGET_FILE:nl_sample_${NL_GL3_SMOKE_SAMPLE}>)
        # TIMEOUT is the watchdog for the script's own helpers: a hung `import` or
        # `xwininfo` would otherwise sit at ctest's default. The script self-limits to
        # ~RUNSECS(14)+4 s, so 40 s can only be reached by a hang.
        SET_TESTS_PROPERTIES(nel_gl3_smoke_${NL_GL3_SMOKE_SAMPLE}
                             PROPERTIES
                             # The sample writes a generated decal texture next to its
                             # working directory, so this must NOT be the source tree
                             # (nl_sample_decal committed one that way).
                             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                             ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
                             TIMEOUT 40)
      ENDIF()
    ENDFOREACH()

    # The Specular sample covers two generated texture-coordinate modes, and the second one is
    # only reachable through a cube-map stage: sphere map (default) and reflection map (--cube).
    # Both are registered, because a regression in the texgen mask reaches the pixel program only
    # through that path (see AGENTS_trackD.md ## D.3).
    IF(TARGET nl_sample_specular)
      ADD_TEST(NAME nel_gl3_smoke_specular_cube
               COMMAND ${NL_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.sh
                       14 $<TARGET_FILE:nl_sample_specular> --cube)
      SET_TESTS_PROPERTIES(nel_gl3_smoke_specular_cube
                           PROPERTIES
                           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                           ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
                           TIMEOUT 40)
      # Alpha test: --cutout implies the Normal shader (the only one with a tex-env path) and
      # turns the surface texture's bands transparent, so the alpha test is what hides them.
      ADD_TEST(NAME nel_gl3_smoke_specular_cutout
               COMMAND ${NL_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.sh
                       14 $<TARGET_FILE:nl_sample_specular> --cutout)
      SET_TESTS_PROPERTIES(nel_gl3_smoke_specular_cutout
                           PROPERTIES
                           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                           ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
                           TIMEOUT 40)
    ENDIF()
  ENDIF()
ENDIF()

#IF(WITH_QT)
#  ADD_SUBDIRECTORY(qtnel)
#ENDIF()
