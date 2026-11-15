FILE(GLOB SRC *.cpp *.h)

SOURCE_GROUP("" FILES ${SRC})

ADD_EXECUTABLE(shape_flip ${SRC})

TARGET_LINK_LIBRARIES(shape_flip nelmisc nel3d)
NL_DEFAULT_PROPS(shape_flip "NeL, Tools, Misc: shape_flip")
NL_ADD_RUNTIME_FLAGS(shape_flip)

INSTALL(TARGETS shape_flip RUNTIME DESTINATION ${NL_BIN_PREFIX} COMPONENT toolsmisc)
