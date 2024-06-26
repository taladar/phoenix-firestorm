# -*- cmake -*-
include(Prebuilt)

include_guard()
add_library( ll::tracy INTERFACE IMPORTED )

set(USE_TRACY OFF CACHE BOOL "Use Tracy profiler.")

if (USE_TRACY)
  use_prebuilt_binary(tracy)

  target_include_directories( ll::tracy SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include/tracy)
  # target_link_libraries( ll::tracy INTERFACE TracyClient )

  # See: indra/llcommon/llprofiler.h
  add_compile_definitions(LL_PROFILER_CONFIGURATION=3)
endif (USE_TRACY)

