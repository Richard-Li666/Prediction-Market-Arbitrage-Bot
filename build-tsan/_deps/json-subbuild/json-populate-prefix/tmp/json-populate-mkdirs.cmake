# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-src")
  file(MAKE_DIRECTORY "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-src")
endif()
file(MAKE_DIRECTORY
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-build"
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix"
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/tmp"
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/src/json-populate-stamp"
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/src"
  "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/src/json-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/src/json-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/zhiyuanli/Documents/Upenn/Spring/CIS3990/FinalProject/final-proj-26sp-cis3990-richard-li666/build-tsan/_deps/json-subbuild/json-populate-prefix/src/json-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
