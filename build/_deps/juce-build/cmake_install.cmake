# Install script for directory: /tmp/hoplite/workspace/build/_deps/juce-src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/tmp/hoplite/workspace/build/_deps/juce-build/modules/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/tmp/hoplite/workspace/build/_deps/juce-build/extras/Build/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/JUCE-9.0.5" TYPE FILE FILES
    "/tmp/hoplite/workspace/build/_deps/juce-build/JUCEConfigVersion.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-build/JUCEConfig.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/FindCppwinrt.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/FindWebView2.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/FindWindowsMIDIServices.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/JUCECheckAtomic.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/JUCEHelperTargets.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/JUCEModuleSupport.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/JUCEUtils.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/JuceLV2Defines.h.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/LaunchScreen.storyboard"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/PIPAudioProcessor.cpp.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/PIPAudioProcessorWithARA.cpp.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/PIPComponent.cpp.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/PIPConsole.cpp.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/RecentFilesMenuTemplate.nib"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/UnityPluginGUIScript.cs.in"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/bundleplaceholder.mm"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/checkBundleSigning.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/copyDir.cmake"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/juce_LinuxSubprocessHelper.cpp"
    "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/juce_runtime_arch_detection.cpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/JUCE-9.0.5" TYPE DIRECTORY FILES "/tmp/hoplite/workspace/build/_deps/juce-src/extras/Build/CMake/juce_vst3_helper")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/tmp/hoplite/workspace/build/_deps/juce-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
