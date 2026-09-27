# Один исполняемый файл на область, регистрируется в CTest по имени файла.
# Новый тест добавляется одной строкой в список ниже.

set(cork_tests
    test_cli
    test_i18n
    test_clpost
    test_doctor
    test_download
    test_exec
    test_fs
    test_generation
    test_json
    test_lock
    test_manifest
    test_msbuild
    test_msi
    test_protocol
    test_resolve
    test_response
    test_session
    test_sha256
    test_shims
    test_store
    test_version
    test_zip
    test_helper_wine
)

foreach(name IN LISTS cork_tests)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE cork_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    cork_set_warnings(${name})
    target_compile_definitions(${name} PRIVATE CORK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# -Wno-c++26-extensions: тест читает вшитый файл тулчейна прямо из
# сгенерированного заголовка, а тот пользуется #embed. Обоснование то же, что
# у src/cli/install.cpp: #embed пришёл из C23 и попадёт в C++ только в C++26,
# так что gcc считает его расширением. Снять вместе с тем, что стоит там.
set_source_files_properties(test_generation.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-c++26-extensions)

# Фикстуры zip собираются тем же libzip, поэтому тесту нужен прямой доступ к
# библиотеке: у cork_core она PRIVATE и наружу не торчит.
target_link_libraries(test_zip PRIVATE libzip::zip)

# Проверкам против настоящего Wine нужны собранные Wine и хелпер, которых
# обычная сборка не производит. Тест сам определяет их наличие и возвращает 77,
# если их нет; CTest трактует это как пропуск, а не как отказ.
set_tests_properties(test_download test_store PROPERTIES TIMEOUT 120)

set_tests_properties(test_helper_wine PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
