cmake_minimum_required(VERSION 3.20)
project(esp_drv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ищем WDK
set(WDK_ROOT "$ENV{ProgramFiles\(x86\)}/Windows Kits/10" CACHE PATH "WDK root")
if (NOT EXISTS "${WDK_ROOT}/Include")
    message(FATAL_ERROR "WDK not found at ${WDK_ROOT}")
endif()

# Берём версию WDK из самой старшей папки Include
file(GLOB WDK_VERSIONS LIST_DIRECTORIES true "${WDK_ROOT}/Include/10.*")
list(SORT WDK_VERSIONS)
list(GET WDK_VERSIONS -1 WDK_VERSION_DIR)
get_filename_component(WDK_VERSION "${WDK_VERSION_DIR}" NAME)
message(STATUS "Using WDK ${WDK_VERSION}")

set(WDK_INC "${WDK_ROOT}/Include/${WDK_VERSION}")
set(WDK_LIB "${WDK_ROOT}/Lib/${WDK_VERSION}/km/x64")

add_executable(esp_drv esp_drv.cpp)

target_include_directories(esp_drv PRIVATE
    "${WDK_INC}/km"
    "${WDK_INC}/shared"
    "${WDK_INC}/ucrt"
)

target_compile_definitions(esp_drv PRIVATE
    _KERNEL_MODE=1
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    POOL_NX_OPTIN=1
    NTDDI_VERSION=0x0A00000C   # Win10 22H2+
    _WIN32_WINNT=0x0A00
)

target_compile_options(esp_drv PRIVATE
    /kernel           # kernel mode
    /GS-              # no stack cookies
    /GR-              # no RTTI
    /EHs-c-           # no C++ exceptions
    /Zl               # no default lib (manual map friendly)
    /O2
    /W3
    /D_AMD64_
)

target_link_options(esp_drv PRIVATE
    /DRIVER
    /SUBSYSTEM:NATIVE
    /ENTRY:DriverEntry
    /NODEFAULTLIB
    /MACHINE:X64
    /IGNORE:4001,4037,4039,4065,4070,4078,4087,4089,4198,4221,4226
)

target_link_libraries(esp_drv PRIVATE
    "${WDK_LIB}/ntoskrnl.lib"
    "${WDK_LIB}/wdm.lib"
    "${WDK_LIB}/BufferOverflowK.lib"
    "${WDK_LIB}/ntstrsafe.lib"
)

# Выход — esp_drv.sys вместо esp_drv.exe
set_target_properties(esp_drv PROPERTIES
    SUFFIX ".sys"
    PREFIX ""
    OUTPUT_NAME "esp_drv"
)
