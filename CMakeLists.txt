cmake_minimum_required(VERSION 3.21)

# По умолчанию собираем Release. В Debug подключаются отладочные библиотеки
# PSn00bSDK, которые печатают по десятку строк на каждое открытие файла.
# В эмуляторе это лишь засоряет лог, а на консоли printf уходит в
# последовательный порт: около 4 мс на строку, до 150 мс лишних задержек
# на смену сцены. Для поиска багов на железе это ещё и лишний источник
# различий с эмулятором.
#   отладочная сборка:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug ...
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "тип сборки" FORCE)
endif()

project(vnengine LANGUAGES C ASM)

psn00bsdk_add_executable(vnengine GPREL
    src/main.c
    src/gfx.c
    src/cd.c
    src/pad.c
    src/seq.c
    src/str.c
    src/vm.c
    src/card.c
    src/gallery.c
    src/save.c
    src/voice.c
)

target_include_directories(vnengine PRIVATE ${PROJECT_SOURCE_DIR}/src)

# -Wundef обязателен: препроцессор считает неизвестное имя в #if нулём и
# молча меняет поведение. Именно так пропавшее определение STR_DOUBLE_SPEED
# однажды перевело чтение видео на одинарную скорость — без единого
# предупреждения. Остальное — обычная гигиена.
target_compile_options(vnengine PRIVATE -Wall -Wundef)

# Релизная сборка: cmake -S . -B build -DVN_RELEASE=ON ...
option(VN_RELEASE "сборка без отладочных средств" OFF)
if(VN_RELEASE)
    target_compile_definitions(vnengine PRIVATE VN_RELEASE=1)
    message(STATUS "релизная сборка: отладочные средства выключены")
endif()

# Данные собираются скриптом на Python до запуска cmake --build
psn00bsdk_add_cd_image(iso vn iso.xml DEPENDS vnengine)
