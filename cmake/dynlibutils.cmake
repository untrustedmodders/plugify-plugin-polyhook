include(FetchContent)

message(STATUS "Pulling and configuring DynLibUtils")

FetchContent_Declare(
		dynlibutils
		GIT_REPOSITORY https://github.com/Wend4r/cpp-memory_utils.git
		GIT_TAG dfda1d93e12945f3373a6309629572a3b1a37156
)

FetchContent_MakeAvailable(dynlibutils)