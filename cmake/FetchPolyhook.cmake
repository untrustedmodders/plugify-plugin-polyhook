include(FetchContent)

message(STATUS "Pulling and configuring PolyHook_2")

FetchContent_Declare(
        PolyHook_2
        GIT_REPOSITORY https://github.com/stevemk14ebr/PolyHook_2_0.git
        GIT_TAG f4aee8e47383825469f924903357038b2efd8ca7
)

FetchContent_MakeAvailable(PolyHook_2)