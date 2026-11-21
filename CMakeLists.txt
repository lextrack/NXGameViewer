cmake_minimum_required(VERSION 3.21)
project(NXGameViewer VERSION 0.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Gui Widgets OpenGL OpenGLWidgets Multimedia MultimediaWidgets Concurrent Test)
if(UNIX AND NOT APPLE)
    find_package(Qt6 COMPONENTS DBus)
endif()
add_library(nxgv_opus INTERFACE)
add_library(nxgv_zstd INTERFACE)
find_package(Opus CONFIG QUIET)
find_package(zstd CONFIG QUIET)
if(TARGET Opus::opus)
    target_link_libraries(nxgv_opus INTERFACE Opus::opus)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(nxgv_opus INTERFACE PkgConfig::OPUS)
endif()
if(TARGET zstd::libzstd)
    target_link_libraries(nxgv_zstd INTERFACE zstd::libzstd)
elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(nxgv_zstd INTERFACE zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    target_link_libraries(nxgv_zstd INTERFACE zstd::libzstd_static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    target_link_libraries(nxgv_zstd INTERFACE PkgConfig::ZSTD)
endif()
qt_standard_project_setup()

add_library(nxgv_warnings INTERFACE)
target_compile_options(nxgv_warnings INTERFACE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/W3,-Wall -Wextra -Wshadow -Wno-missing-field-initializers>)

add_library(formats STATIC
    src/formats/core/audio.cpp
    src/formats/core/batchexport.cpp  src/formats/core/batchexport.h
    src/formats/core/bytes.h
    src/formats/core/contentkind.cpp  src/formats/core/contentkind.h
    src/formats/core/paths.cpp  src/formats/core/paths.h
    src/formats/core/format.h
    src/formats/core/records.cpp  src/formats/core/records.h
    src/formats/core/registry.cpp  src/formats/core/registry.h
    src/formats/core/schema.cpp  src/formats/core/schema.h
    src/formats/core/simpletable.cpp  src/formats/core/simpletable.h
    src/formats/core/storedreader.cpp  src/formats/core/storedreader.h
    src/formats/core/textdecode.cpp  src/formats/core/textdecode.h
    src/formats/core/treetable.cpp  src/formats/core/treetable.h
    src/formats/core/zipwriter.cpp  src/formats/core/zipwriter.h
    src/formats/archives/compress.cpp  src/formats/archives/compress.h
    src/formats/archives/zstddictionaries.cpp  src/formats/archives/zstddictionaries.h
    src/formats/archives/decompress.cpp  src/formats/archives/decompress.h
    src/formats/archives/sarcreader.cpp  src/formats/archives/sarcreader.h
    src/formats/archives/sarcwriter.cpp  src/formats/archives/sarcwriter.h
    src/formats/archives/rpf6reader.cpp  src/formats/archives/rpf6reader.h  src/formats/archives/rpf6key.cpp
    src/formats/textures/astc.cpp  src/formats/textures/astc.h
    src/formats/textures/bntxreader.cpp  src/formats/textures/bntxreader.h
    src/formats/textures/ddsreader.cpp  src/formats/textures/ddsreader.h
    src/formats/textures/bntxwriter.cpp  src/formats/textures/bntxwriter.h
    src/formats/textures/texturedecode.cpp  src/formats/textures/texturedecode.h
    src/formats/textures/textureencode.cpp  src/formats/textures/textureencode.h
    src/formats/textures/texturemips.cpp  src/formats/textures/texturemips.h
    src/formats/textures/texturesearch.cpp  src/formats/textures/texturesearch.h
    src/formats/textures/xtxreader.cpp  src/formats/textures/xtxreader.h
    src/formats/textures/xtxwriter.cpp  src/formats/textures/xtxwriter.h
    src/formats/models/animationsearch.cpp  src/formats/models/animationsearch.h
    src/formats/models/bfresreader.cpp  src/formats/models/bfresreader.h
    src/formats/models/falcommdlreader.cpp  src/formats/models/falcommdlreader.h
    src/formats/models/gltfwriter.cpp  src/formats/models/gltfwriter.h
    src/formats/models/meshpreview.cpp  src/formats/models/meshpreview.h
    src/formats/models/model3d.cpp  src/formats/models/model3d.h
    src/formats/sound/barsreader.cpp  src/formats/sound/barsreader.h
    src/formats/sound/bwavreader.cpp  src/formats/sound/bwavreader.h
    src/formats/sound/wavreader.cpp  src/formats/sound/wavreader.h
    src/formats/sound/nxopusreader.cpp  src/formats/sound/nxopusreader.h
    src/formats/data/bcsvreader.cpp  src/formats/data/bcsvreader.h
    src/formats/data/restblreader.cpp  src/formats/data/restblreader.h
    src/formats/data/bymlwriter.cpp  src/formats/data/bymlwriter.h
    src/formats/data/msbtwriter.cpp  src/formats/data/msbtwriter.h
    src/formats/data/nintendofont.cpp  src/formats/data/nintendofont.h
    src/formats/data/ainbreader.cpp  src/formats/data/ainbreader.h
    src/formats/data/evflreader.cpp  src/formats/data/evflreader.h
    src/formats/data/asbreader.cpp  src/formats/data/asbreader.h
    src/formats/data/baevreader.cpp  src/formats/data/baevreader.h
    src/formats/data/terrainreader.cpp  src/formats/data/terrainreader.h
    src/formats/data/bymlreader.cpp  src/formats/data/bymlreader.h
    src/formats/data/ed9bytecode.cpp  src/formats/data/ed9bytecode.h
    src/formats/data/falcomjsonreader.cpp  src/formats/data/falcomjsonreader.h
    src/formats/data/falcomscriptreader.cpp  src/formats/data/falcomscriptreader.h
    src/formats/data/falcomtblreader.cpp  src/formats/data/falcomtblreader.h
    src/formats/data/hexreader.cpp  src/formats/data/hexreader.h
    src/formats/data/jsonreader.cpp  src/formats/data/jsonreader.h
    src/formats/data/msbtreader.cpp  src/formats/data/msbtreader.h
    src/formats/data/nrbfreader.cpp  src/formats/data/nrbfreader.h
    src/formats/data/tblfile.cpp  src/formats/data/tblfile.h
    src/formats/data/textreader.cpp  src/formats/data/textreader.h
    src/formats/modding/modlayout.cpp  src/formats/modding/modlayout.h
    src/formats/modding/modlibrary.cpp  src/formats/modding/modlibrary.h
    src/formats/modding/nestededit.cpp  src/formats/modding/nestededit.h
    src/formats/modding/restblupdate.cpp  src/formats/modding/restblupdate.h
    src/formats/modding/editedjson.cpp  src/formats/modding/editedjson.h
    src/formats/cri/criutf.cpp  src/formats/cri/criutf.h
    src/formats/cri/crireader.cpp  src/formats/cri/crireader.h
    src/formats/cri/usmreader.cpp  src/formats/cri/usmreader.h
    src/formats/unity/lz4block.cpp  src/formats/unity/lz4block.h
    src/formats/unity/serializedfile.cpp  src/formats/unity/serializedfile.h
    src/formats/unity/unitybundle.cpp  src/formats/unity/unitybundle.h
    src/formats/unity/unityreader.cpp  src/formats/unity/unityreader.h
    src/formats/unity/unitytexture.cpp  src/formats/unity/unitytexture.h
    src/formats/unity/unitymesh.cpp  src/formats/unity/unitymesh.h
    src/formats/unity/unityscene.cpp  src/formats/unity/unityscene.h
    src/formats/unity/builtintypetrees.cpp  src/formats/unity/builtintypetrees.h
    src/formats/rage/rageresource.cpp  src/formats/rage/rageresource.h
    src/formats/rage/ragetexturereader.cpp  src/formats/rage/ragetexturereader.h
    src/formats/rage/ragestringtablereader.cpp  src/formats/rage/ragestringtablereader.h
    src/formats/rage/ragemodelreader.cpp  src/formats/rage/ragemodelreader.h
    src/formats/rage/rageanimation.cpp  src/formats/rage/rageanimation.h
    src/formats/rage/awcreader.cpp  src/formats/rage/awcreader.h
    src/formats/core/crypto.cpp  src/formats/core/crypto.h
    src/formats/switchfs/switchkeys.cpp  src/formats/switchfs/switchkeys.h
    src/formats/switchfs/switchkeysearch.cpp  src/formats/switchfs/switchkeysearch.h
    src/formats/switchfs/switchnca.cpp  src/formats/switchfs/switchnca.h
    src/formats/switchfs/switchnup.cpp  src/formats/switchfs/switchnup.h
    src/formats/switchfs/switchncz.cpp  src/formats/switchfs/switchncz.h
    src/formats/switchfs/switchpackagereader.cpp  src/formats/switchfs/switchpackagereader.h
    src/formats/switchfs/switchgame.cpp  src/formats/switchfs/switchgame.h
)
add_library(bc7enc STATIC third_party/bc7enc/bc7enc.c)
add_library(bcdec STATIC third_party/bcdec/bcdec.c)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
file(GLOB ASTCENC_SOURCES CONFIGURE_DEPENDS third_party/astcenc/astcenc_*.cpp)
add_library(astcenc STATIC ${ASTCENC_SOURCES})
target_include_directories(astcenc SYSTEM PUBLIC third_party/astcenc)
set_target_properties(astcenc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(astcenc PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
find_package(Threads REQUIRED)
target_link_libraries(astcenc PUBLIC Threads::Threads)
target_include_directories(bc7enc SYSTEM PUBLIC third_party/bc7enc)
set_target_properties(bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(bc7enc PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/O2,-O2>)
target_include_directories(bcdec SYSTEM PUBLIC third_party/bcdec)
set_target_properties(bcdec PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(bcdec PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/O2,-O2>)

target_include_directories(formats PUBLIC src src/formats)
target_link_libraries(formats PUBLIC Qt6::Core PRIVATE nxgv_opus nxgv_zstd bc7enc bcdec astcenc ZLIB::ZLIB OpenSSL::Crypto nxgv_warnings)

add_library(mediaformats STATIC
    src/formats/media/mediaformats.cpp     src/formats/media/mediaformats.h
    src/formats/media/imagefilereader.cpp  src/formats/media/imagefilereader.h
    src/formats/media/audiofilereader.cpp  src/formats/media/audiofilereader.h
    src/formats/media/fontfilereader.cpp  src/formats/media/fontfilereader.h
    src/formats/media/videofilereader.cpp  src/formats/media/videofilereader.h
)
target_link_libraries(mediaformats PUBLIC formats Qt6::Gui Qt6::Multimedia PRIVATE nxgv_warnings)

add_library(viewerui STATIC
    src/ui/mainwindow.cpp src/ui/mainwindow.h
    src/ui/mainwindow_browser.cpp src/ui/mainwindow_columns.cpp src/ui/mainwindow_export.cpp src/ui/mainwindow_keys.cpp
    src/ui/mainwindow_mod.cpp
    src/ui/audioplayer.cpp src/ui/audioplayer.h
    src/ui/videoview.cpp src/ui/videoview.h
    src/ui/tablemodel.cpp src/ui/tablemodel.h
    src/ui/imageview.cpp src/ui/imageview.h
    src/ui/modelview.cpp src/ui/modelview.h
    src/ui/animationdialog.cpp src/ui/animationdialog.h
    src/ui/batchexportdialog.cpp src/ui/batchexportdialog.h
    src/ui/newmoddialog.cpp src/ui/newmoddialog.h
    src/ui/modmanagerdialog.cpp src/ui/modmanagerdialog.h src/ui/modhistory.h
    src/ui/gameextractdialog.cpp src/ui/gameextractdialog.h
    src/ui/fasticonprovider.h
    src/ui/progressdialogs.h
    src/ui/hoverdelegate.h
    src/ui/contenticons.cpp src/ui/contenticons.h
)
target_link_libraries(viewerui PUBLIC formats mediaformats Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia Qt6::MultimediaWidgets Qt6::Concurrent
    PRIVATE nxgv_warnings)
if(TARGET Qt6::DBus)
    target_link_libraries(viewerui PRIVATE Qt6::DBus)
    target_compile_definitions(viewerui PRIVATE HAVE_QT_DBUS)
endif()

qt_add_executable(NXGameViewer src/main.cpp)
target_link_libraries(NXGameViewer PRIVATE viewerui nxgv_warnings)
set_target_properties(NXGameViewer PROPERTIES WIN32_EXECUTABLE TRUE)
include(GNUInstallDirs)
install(TARGETS NXGameViewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
foreach(test test_tblfile test_formats test_falcomjson test_records test_textures test_falcomscript test_text
             test_models test_audio test_batch test_archives test_texturewrite test_modlayout test_archivewrite test_bntx test_mips test_astc test_bfres test_gamedata test_sound test_unity test_cri test_rpf6 test_rage test_switch)
    qt_add_executable(${test} tests/${test}.cpp tests/sample_tbl.h tests/sample_bjson.h tests/sample_mdl.h tests/sample_sarc.h tests/sample_bfres.h tests/switch_samples.h)
    target_link_libraries(${test} PRIVATE formats Qt6::Test nxgv_warnings)
    add_test(NAME ${test} COMMAND ${test})
endforeach()
target_link_libraries(test_audio PRIVATE nxgv_opus)
target_link_libraries(test_cri PRIVATE nxgv_opus)
target_link_libraries(test_sound PRIVATE nxgv_opus)
target_link_libraries(test_rpf6 PRIVATE ZLIB::ZLIB)
target_link_libraries(test_models PRIVATE Qt6::Gui)
target_link_libraries(test_archives PRIVATE nxgv_zstd)
target_link_libraries(test_archivewrite PRIVATE nxgv_zstd)
target_link_libraries(test_switch PRIVATE nxgv_zstd)
target_link_libraries(test_modlayout PRIVATE ZLIB::ZLIB)

qt_add_executable(test_media tests/test_media.cpp)
target_link_libraries(test_media PRIVATE mediaformats Qt6::Concurrent Qt6::Test nxgv_warnings)
add_test(NAME test_media COMMAND test_media)
set_tests_properties(test_media PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0")

qt_add_executable(test_mainwindow tests/test_mainwindow.cpp tests/sample_tbl.h tests/sample_mdl.h)
target_link_libraries(test_mainwindow PRIVATE viewerui Qt6::Test ZLIB::ZLIB nxgv_zstd nxgv_warnings)
add_test(NAME test_mainwindow COMMAND test_mainwindow)
set_tests_properties(test_mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0")

option(NXGV_FUZZ "Build the libFuzzer targets (needs clang)" OFF)
if(NXGV_FUZZ)
    add_executable(fuzz_readers fuzz/fuzz_readers.cpp)
    target_link_libraries(fuzz_readers PRIVATE formats nxgv_warnings)
    target_link_options(fuzz_readers PRIVATE -fsanitize=fuzzer)
    add_executable(make_seeds fuzz/make_seeds.cpp)
    target_include_directories(make_seeds PRIVATE tests)
    target_link_libraries(make_seeds PRIVATE formats ZLIB::ZLIB nxgv_warnings)
endif()
