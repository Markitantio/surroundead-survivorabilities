file(GLOB_RECURSE SMARTMEDIC_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(SmartMedic SHARED ${SMARTMEDIC_SOURCES})

set_target_properties(SmartMedic PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
)

target_include_directories(SmartMedic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/UE4SS/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/UE4SS/generated_include
    ${CMAKE_BINARY_DIR}/RE-UE4SS/UE4SS/generated
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/File/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Helpers/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/String/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/DynamicOutput/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Unreal/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Unreal/generated_include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/SinglePassSigScanner/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Constructs/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Function/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/ASMHelper/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/LuaMadeSimple/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/LuaRaw/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/IniParser/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/ParserBase/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/JSON/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Input/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/MProgram/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/ScopedTimer/include
    ${CMAKE_SOURCE_DIR}/RE-UE4SS/deps/first/Profiler/include
)

# Транзитивные include/link-зависимости — как у AutoSortLoot
target_link_libraries(SmartMedic PRIVATE UE4SS)