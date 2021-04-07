cmake_minimum_required (VERSION 2.6)
project (IntelMediaSDKSample)

# Set a default build type if none was specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'Debug' as none was specified.")
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
    "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall") 
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
add_definitions(-D__cplusplus=201103L)

set(MEDIA_SDK_PATH "/opt/intel" CACHE STRING "media sdk path")

include_directories(
	"${MEDIA_SDK_PATH}/libva/include/"
	"${MEDIA_SDK_PATH}/mediasdk/include/mfx"
)

link_directories(
	"${MEDIA_SDK_PATH}/libva/lib"
	"${MEDIA_SDK_PATH}/mediasdk/lib64"
)

file(GLOB src
  	"${CMAKE_CURRENT_SOURCE_DIR}/src/*"
)

add_library (IntelMediaSDKSample SHARED ${src})
target_link_libraries (IntelMediaSDKSample
	va
	va-drm
	mfxhw64
)
