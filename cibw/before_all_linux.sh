#!/usr/bin/env sh
set -e

if command -v dnf >/dev/null 2>&1; then
    dnf -y install cmake openssl3-devel libXrandr-devel
    export PKG_CONFIG_PATH="${PKG_CONFIG_PATH}:/usr/lib64/pkgconfig"
    ln -s /usr/lib64/pkgconfig/openssl3.pc /usr/lib64/pkgconfig/openssl.pc
    curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal
    source ~/.cargo/env
    cargo install cargo-c
else
    apk add --no-cache cmake rust cargo cargo-c libxrandr-dev
fi

uv tool install meson
uv tool install ninja
export PATH="${PATH}:$HOME/.local/bin"

git clone https://github.com/KhronosGroup/Vulkan-Headers.git --branch "${VULKAN_SDK_TAG}" --depth 1
cd Vulkan-Headers
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
cmake --install build
cd ..
rm -rf Vulkan-Headers

git clone https://github.com/KhronosGroup/Vulkan-Loader --branch "${VULKAN_SDK_TAG}" --depth 1
cd Vulkan-Loader
cmake -B build -D CMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    -DCMAKE_SKIP_INSTALL_RPATH=ON
cmake --build build
cmake --install build
cd ..
rm -rf Vulkan-Loader

git clone https://github.com/KhronosGroup/SPIRV-Headers.git --branch "${VULKAN_SDK_TAG}" --depth 1
cd SPIRV-Headers
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
cmake --install build
cd ..
rm -rf SPIRV-Headers

git clone https://github.com/KhronosGroup/SPIRV-Tools.git --branch "${VULKAN_SDK_TAG}" --depth 1
cd SPIRV-Tools
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_SHARED_LIBS=ON \
    -DSPIRV_TOOLS_BUILD_STATIC=OFF \
    -DCMAKE_INSTALL_SYSCONFDIR=/etc \
    -DSPIRV-Headers_SOURCE_DIR=/usr
cmake --build build
cmake --install build
cd ..
rm -rf SPIRV-Tools

git clone https://github.com/KhronosGroup/SPIRV-Cross.git --branch "${VULKAN_SDK_TAG}" --depth 1
cd SPIRV-Cross
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DSPIRV_CROSS_SHARED=ON \
    -DSPIRV_CROSS_STATIC=OFF \
    -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
cmake --install build
cd ..
rm -rf SPIRV-Cross

git clone https://github.com/KhronosGroup/glslang.git --branch "${VULKAN_SDK_TAG}" --depth 1
cd glslang
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DALLOW_EXTERNAL_SPIRV_TOOLS='ON' \
    -DBUILD_SHARED_LIBS='ON'
cmake --build build
cmake --install build
cd ..
rm -rf glslang

git clone https://github.com/google/shaderc.git --branch "${SHADERC_TAG}" --depth 1
cd shaderc
sed '/examples/d;/third_party/d' -i CMakeLists.txt
sed '/build-version/d' -i glslc/CMakeLists.txt
printf "\"${SHADERC_VERSION}\"\n\"${VULKAN_SDK_VERSION}\"\n\"${VULKAN_SDK_VERSION}\"\n" > glslc/src/build-version.inc
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DSHADERC_SKIP_TESTS=ON \
    -Dglslang_SOURCE_DIR=/usr/include/glslang
cmake --build build
cmake --install build
cd ..
rm -rf shaderc

git clone https://github.com/quietvoid/dovi_tool.git --branch "${LIBDOVI_TAG}" --depth 1
cd dovi_tool/dolby_vision
cargo fetch --locked
cargo cbuild --frozen --profile release-deploy \
    --library-type cdylib \
    --prefix=/usr
cargo cinstall --frozen --profile release-deploy \
    --library-type cdylib \
    --prefix=/usr
cd ../..
rm -rf dovi_tool

git clone https://github.com/haasn/libplacebo.git --branch "${LIBPLACEBO_TAG}" --depth 1
cd libplacebo
git submodule update --init
meson setup build --buildtype=release --prefix=/usr \
    -Dvulkan=enabled \
    -Dglslang=enabled \
    -Dshaderc=enabled
meson compile -C build
meson install -C build
cd ..
rm -rf libplacebo
