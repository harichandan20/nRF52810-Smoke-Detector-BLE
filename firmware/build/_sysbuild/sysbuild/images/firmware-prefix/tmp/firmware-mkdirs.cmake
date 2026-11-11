# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware")
  file(MAKE_DIRECTORY "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware")
endif()
file(MAKE_DIRECTORY
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/firmware"
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix"
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/tmp"
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/src/firmware-stamp"
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/src"
  "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/src/firmware-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/src/firmware-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/hari2/OneDrive/Desktop/nRF52810-Smoke-Detector-BLE/firmware/build/_sysbuild/sysbuild/images/firmware-prefix/src/firmware-stamp${cfgdir}") # cfgdir has leading slash
endif()
