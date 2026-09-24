/**
 * @file    Api.h
 * @brief   Макрос экспорта публичного API движка (статика / DLL).
 *
 * Сборка LimVine по умолчанию статическая, и макрос раскрывается в пустоту.
 * При -DLV_BUILD_SHARED=ON CMake определяет лимvine_EXPORTS автоматически
 * (генератором Win32 — как LIMVINE_EXPORTS, компилятором ELF — как
 * limvine_EXPORTS), поэтому проверяются оба имени.
 *   * при компиляции самой библиотеки → LV_API = __declspec(dllexport) /
 *     __attribute__((visibility("default")));
 *   * у потребителя LV_API = __declspec(dllimport) на MSVC;
 *   * на GCC/Clang все символы скрыты (-fvisibility=hidden), поэтому без
 *     LV_API класс/функция не попадёт в таблицу экспорта DLL/.so — это и
 *     есть «production-контракт»: наружу смотрит только помеченное API.
 *
 * Правила:
 *   1. Помечать LV_API можно только целыми классами или free-функциями,
 *      определёнными в .cpp. Шаблонные сущности и inline-хелперы помечать
 *      бессмысленно — они инстанцируются на стороне потребителя.
 *   2. Через границу DLL нельзя передавать объекты со static-памятью CRT
 *      (std::string/std::wostream) между модулями, собранными с РАЗНЫМ CRT;
 *      потребители обязаны использовать тот же набор /MD, что и сборка
 *      (см. CMAKE_MSVC_RUNTIME_LIBRARY в корневом CMakeLists.txt).
 */
#pragma once

#if defined(LIMVINE_STATIC) || !defined(LV_BUILD_SHARED)
#  define LV_API
#else
#  if defined(_WIN32) || defined(__CYGWIN__)
#    if defined(LIMVINE_EXPORTS) || defined(limvine_EXPORTS)
#      define LV_API __declspec(dllexport)
#    else
#      define LV_API __declspec(dllimport)
#    endif
#  else
#    if defined(LIMVINE_EXPORTS) || defined(limvine_EXPORTS)
#      define LV_API __attribute__((visibility("default")))
#    else
#      define LV_API
#    endif
#  endif
#endif
