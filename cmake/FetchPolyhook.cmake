include(FetchContent)

message(STATUS "Pulling and configuring PolyHook_2")

FetchContent_Declare(
        PolyHook_2
        GIT_REPOSITORY https://github.com/stevemk14ebr/PolyHook_2_0.git
        GIT_TAG 298d56210b9d9e66cde8f96481d6053925c6ae15
)

FetchContent_MakeAvailable(PolyHook_2)