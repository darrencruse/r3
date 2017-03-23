#
# .travis.yaml contains YAML-formatted (http://www.yaml.org/) build
# instructions for continuous integration via Travis CI
# (http://docs.travis-ci.com/).
#

notifications:
    email: false

language: c

matrix:
    include:
        # Linux x86, release
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.4 BUILD_TYPE=RELEASE R3_CPP=0 CFLAGS=-m32 CXXFLAGS=-m32 EXTRA_CMAKE_ARGS="-DCMAKE_ASM_FLAGS=-m32"

        # Linux x86, debug
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.4 BUILD_TYPE=DEBUG R3_CPP=0 CFLAGS=-m32 CXXFLAGS=-m32 EXTRA_CMAKE_ARGS="-DCMAKE_ASM_FLAGS=-m32"

        # Linux x86, debug, build with CPP
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.4 BUILD_TYPE=DEBUG R3_CPP=1 CFLAGS=-m32 CXXFLAGS=-m32 EXTRA_CMAKE_ARGS="-DCMAKE_ASM_FLAGS=-m32"

        # Linux x64, debug
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.40 BUILD_TYPE=DEBUG R3_CPP=0

        # Linux x64, debug, build with CPP
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.40 BUILD_TYPE=DEBUG R3_CPP=1

        # Linux x64, release
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.4.40 BUILD_TYPE=RELEASE R3_CPP=0

        # Windows x86, release
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.1 EXE_SUFFIX=.exe BUILD_TYPE=RELEASE R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw32-linux.cmake"
        # Windows x86, debug
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.1 EXE_SUFFIX=.exe BUILD_TYPE=DEBUG R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw32-linux.cmake"
        # Windows x86, debug, CPP
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.1 EXE_SUFFIX=.exe BUILD_TYPE=DEBUG R3_CPP=1 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw32-linux.cmake"
        # Windows x64, release
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.40 EXE_SUFFIX=.exe BUILD_TYPE=RELEASE R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw64-linux.cmake"
        # Windows x64, debug
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.40 EXE_SUFFIX=.exe BUILD_TYPE=DEBUG R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw64-linux.cmake"
        # Windows x64, debug, CPP
        - os: linux
          sudo: required
          dist: trusty
          env: OS_ID=0.3.40 EXE_SUFFIX=.exe BUILD_TYPE=DEBUG R3_CPP=1 EXTRA_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=Toolchain-cross-mingw64-linux.cmake"
        # OSX x86
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.5 BUILD_TYPE=RELEASE R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_ASM_FLAGS=\"-arch i386\""
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.5 BUILD_TYPE=DEBUG R3_CPP=0 EXTRA_CMAKE_ARGS="-DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_ASM_FLAGS=\"-arch i386\""
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.5 BUILD_TYPE=DEBUG R3_CPP=1 EXTRA_CMAKE_ARGS="-DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_ASM_FLAGS=\"-arch i386\""
        # OSX x64
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.40 BUILD_TYPE=RELEASE R3_CPP=0
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.40 BUILD_TYPE=DEBUG R3_CPP=0
        - os: osx
          osx_image: xcode8
          env: OS_ID=0.2.40 BUILD_TYPE=DEBUG R3_CPP=1

addons:
    apt:
        packages:
            # For building 32b binaries on a 64b host (not necessary when we
            # build for 64b):
            - gcc-multilib
            - g++-multilib

            # For cross-compiling to Windows.
            - binutils-mingw-w64-i686
            - binutils-mingw-w64-x86-64
            - gcc-mingw-w64-i686
            - gcc-mingw-w64-x86-64
            - g++-mingw-w64-i686
            - g++-mingw-w64-x86-64
            - mingw-w64


install:
    # Fetch a Rebol bootstrap binary, which is needed for building Rebol.
    #- wget http://www.rebol.com/r3/downloads/r3-a111-4-2.tar.gz
    #- tar xvzf r3-a111-4-2.tar.gz
    - if [[ "$TRAVIS_OS_NAME" == "linux" ]]; then wget http://www.rebolsource.net/downloads/experimental/r3-linux-x64-gbf237fc-static && cp r3-linux-x64-gbf237fc-static make/r3-make; fi
    - if [[ "$TRAVIS_OS_NAME" == "osx" ]]; then wget http://rebolsource.net/downloads/experimental/r3-osx-x64-gbf237fc && cp r3-osx-x64-gbf237fc make/r3-make; fi
    - chmod +x make/r3-make

script:
    - cd make/
    #compile tcc as a cross-compiler
    - if [ "${OS_ID}" = "0.3.1" ]; then mkdir tcc-build && cd tcc-build && cmake -DTCC_BUILD_WIN32=1 -G "Unix Makefiles" ../../external/tcc && make i386-w64-mingw32-tcc i386-w64-mingw32-libtcc1 VERBOSE=1 && cp i386-w64-mingw32-tcc ../cross-tcc && cp i386-w64-mingw32-libtcc1.a ../cross-libtcc1.a && cd .. && rm -fr tcc-build; fi
    - if [ "${OS_ID}" = "0.3.40" ]; then mkdir tcc-build && cd tcc-build && cmake -DTCC_BUILD_WIN64=1 -G "Unix Makefiles" ../../external/tcc && make x86_64-w64-mingw32-tcc x86_64-w64-mingw32-libtcc1 VERBOSE=1 && cp x86_64-w64-mingw32-tcc ../cross-tcc && cp x86_64-w64-mingw32-libtcc1.a ../cross-libtcc1.a && cd .. && rm -fr tcc-build; fi
    - cmake -DR3_OS_ID="${OS_ID}" -DR3_EXTERNAL_FFI=0 -DR3_CPP="${R3_CPP}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DR3_WITH_TCC=1 -G "Unix Makefiles" "${EXTRA_CMAKE_ARGS}"
    - make r3-core VERBOSE=1
    # A minimal sanity check that the built R3 does _something_. Eventually, we
    # should run the full test suite.

    # Run once but don't pipe output, in case it prints out useful crash msg
    - if [ "${OS_ID}" = "0.4.40" ]; then ./r3-core --do 'print {OK}'; fi

    # Run a second time with piped output to return success/faiure to Travis
    - if [ "${OS_ID}" = "0.4.40" ]; then ./r3-core --do 'print {OK}' | grep OK; fi

    # overwriting libtcc1.a with the cross-compiled version
    - if [ "${OS_ID}" = "0.3.1" -o "${OS_ID}" = "0.3.40" ]; then cp cross-libtcc1.a tcc/libtcc1.a; fi

    # Rename files before uploading
    - zip r3-core-${OS_ID}-${TRAVIS_COMMIT}-${BUILD_TYPE}-CPP${R3_CPP}.zip r3-core${EXE_SUFFIX} tcc/libtcc1.a

deploy:
  provider: releases
  api_key:
    secure: V6a5VzBv+ut3hKZKMmnuY4Urzc4QA/EBcfarve837q+7p9QgDseiuW93yUVys7LacIl8D6y13m71QBxzG6LC9WnttNgfy+PfyrMbWfaMvg9zLQQ1jTGKWjW6Fn4/xyU0NYyrjvgxW2itQ4/r9r0lcmKHbsAcm/ZhvLzg4o3dnc0=
  file:
      - r3-core-${OS_ID}-${TRAVIS_COMMIT}-${BUILD_TYPE}-CPP${R3_CPP}.zip
  skip_cleanup: true #or, Travis CI deletes all the files created during the build
  on:
    tags: true
