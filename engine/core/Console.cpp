/**
 * @file    Console.cpp
 * @brief   Реализация инициализации консоли (см. Console.h).
 */
#include "core/Console.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lv {
namespace core {

void initConsole() {
#if defined(_WIN32)
    // UTF-8 для вывода и ввода консоли. Вызывается один раз за процесс;
    // повторные вызовы идемпотентны (просто выставляют те же CP).
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    // ANSI escape-последовательности (цвета, курсор) через VT-режим.
    // Работает на Windows 10 1511+; на старых системах вызов просто
    // завершается ошибкой, которую мы молча игнорируем — вывод остаётся
    // читаемым, только без раскраски.
    const HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD mode = 0;
        if (::GetConsoleMode(hOut, &mode)) {
            ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
}

} // namespace core
} // namespace lv
