# fmt dependency setup (header-only).
#
# Ensures the fmt::fmt-header-only target is available by finding a pre-installed
# version (either via CMake config or header discovery).

include_guard(GLOBAL)

# in case the include is moved before finding HSDK, try to find it, but not required and no need to configure
if (NOT holoscan_FOUND)
  find_package(holoscan 4.0 PATHS "/opt/nvidia/holoscan")
endif()

# try to find HSDK's version first.
find_package(fmt 11 QUIET)
if (NOT fmt_FOUND)
  # fallback to the system version
  find_package(fmt 8 QUIET)
endif()

if (NOT fmt_FOUND)
  # Last resort (e.g. bare-metal native-only builds without HSDK or
  # libfmt-dev): fetch fmt and use it header-only, matching how this
  # project consumes fmt exclusively through fmt::fmt-header-only.
  set(HOLOLINK_FMT_VERSION "11.1.4")
  message(STATUS "fmt: not found via find_package; fetching v${HOLOLINK_FMT_VERSION}")
  include(FetchContent)
  # SOURCE_SUBDIR points at a directory without a CMakeLists.txt so
  # FetchContent_MakeAvailable only populates the sources and never
  # add_subdirectory()s fmt's own project. The header-only target is then
  # defined as an IMPORTED interface — imported targets may stay out of the
  # HololinkTargets export set (a build-tree fmt::fmt-header-only library
  # target could not), mirroring what find_package(fmt) provides.
  FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG ${HOLOLINK_FMT_VERSION}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _no_cmakelists_
  )
  FetchContent_MakeAvailable(fmt)
  add_library(hololink_fmt_header_only INTERFACE IMPORTED GLOBAL)
  set_target_properties(hololink_fmt_header_only PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${fmt_SOURCE_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS "FMT_HEADER_ONLY=1")
  add_library(fmt::fmt-header-only ALIAS hololink_fmt_header_only)
  message(STATUS "fmt: using fetched v${HOLOLINK_FMT_VERSION} header-only from ${fmt_SOURCE_DIR}")
else()
  message(STATUS "Found fmt: ${fmt_DIR} (found version \"${fmt_VERSION}\")")
endif()