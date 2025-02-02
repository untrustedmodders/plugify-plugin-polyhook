include(FetchContent)

message(STATUS "Pulling and configuring PolyHook_2")

FetchContent_Declare(
        PolyHook_2
        GIT_REPOSITORY https://github.com/stevemk14ebr/PolyHook_2_0.git
        GIT_TAG 19e7cec8cce4a0068f6db04b6d3680c078183002
)

set(FMT_DOC OFF CACHE INTERNAL "Generate the doc target.")
set(FMT_INSTALL OFF CACHE INTERNAL "Generate the install target.")
set(FMT_TEST OFF CACHE INTERNAL "Generate the test target.")
set(FMT_FUZZ OFF CACHE INTERNAL "Generate the fuzz target.")
set(FMT_CUDA_TEST OFF CACHE INTERNAL "Generate the cuda-test target.")

FetchContent_MakeAvailable(PolyHook_2)