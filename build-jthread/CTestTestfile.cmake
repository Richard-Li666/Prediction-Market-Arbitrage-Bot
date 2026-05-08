# CMake generated Testfile for 
# Source directory: /Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666
# Build directory: /Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-jthread
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(thread_pool_smoke "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-jthread/thread_pool_smoke")
set_tests_properties(thread_pool_smoke PROPERTIES  _BACKTRACE_TRIPLES "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/CMakeLists.txt;165;add_test;/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/CMakeLists.txt;0;")
subdirs("_deps/json-build")
subdirs("_deps/ixwebsocket-build")
