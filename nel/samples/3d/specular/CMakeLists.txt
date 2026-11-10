SET(SPECULAR_SOURCES main.cpp)

ADD_EXECUTABLE(nl_sample_specular ${SPECULAR_SOURCES})

TARGET_LINK_LIBRARIES(nl_sample_specular NeL::3d NeL::misc)

NL_DEFAULT_PROPS(nl_sample_specular "NeL, 3D, Samples: Specular material shader")
NL_ADD_RUNTIME_FLAGS(nl_sample_specular)
