# CMake generated Testfile for 
# Source directory: /tmp/hoplite/workspace
# Build directory: /tmp/hoplite/workspace/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("ReverseSamplerAudioRegression" "/tmp/hoplite/workspace/build/ReverseSamplerTests_artefacts/Release/ReverseSamplerTests")
set_tests_properties("ReverseSamplerAudioRegression" PROPERTIES  TIMEOUT "600" _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;132;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("EqDsp" "/tmp/hoplite/workspace/build/EqDspTests")
set_tests_properties("EqDsp" PROPERTIES  _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;137;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("DriveDsp" "/tmp/hoplite/workspace/build/DriveDspTests")
set_tests_properties("DriveDsp" PROPERTIES  _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;141;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("ChorusDsp" "/tmp/hoplite/workspace/build/ChorusDspTests")
set_tests_properties("ChorusDsp" PROPERTIES  _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;145;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("CompressorDsp" "/tmp/hoplite/workspace/build/CompressorDspTests")
set_tests_properties("CompressorDsp" PROPERTIES  _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;149;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("ReverbDsp" "/tmp/hoplite/workspace/build/ReverbDspTests")
set_tests_properties("ReverbDsp" PROPERTIES  TIMEOUT "600" _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;153;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
add_test("IdleGate" "/tmp/hoplite/workspace/build/IdleGateTests")
set_tests_properties("IdleGate" PROPERTIES  _BACKTRACE_TRIPLES "/tmp/hoplite/workspace/CMakeLists.txt;157;add_test;/tmp/hoplite/workspace/CMakeLists.txt;0;")
subdirs("_deps/juce-build")
