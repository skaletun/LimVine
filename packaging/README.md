# LimVine — сборка релиза (Windows-инсталлятор + Linux-пакеты)

Два workflow: `ci.yml` прогоняет тесты на каждом push/PR, а релизный пакет
собирается по тегу `v*`. Инсталлятор Windows генерируется CPack/NSIS прямо
на windows-latest раннере — там же, где и сама сборка MSVC.

```
Windows: cpack -G NSIS  → LimVine-x.y.z-win64.exe  (мастер установки + uninstall.exe)
Linux:   cpack -G TGZ;DEB → .tar.gz / .deb
```

## Что делает инсталлятор (NSIS из коробки CPack)

* мастер установки с выбором каталога (по умолчанию `%ProgramFiles%\LimVine`);
* ярлыки в меню «Пуск»: lvrun, lvcook (см. `CPACK_NSIS_MENU_LINKS`);
* запись в «Программы и компоненты» (Add/Remove Programs) с корректной
  деинсталляцией — `uninstall.exe` создаётся автоматически;
* опция «Add to PATH» (`CPACK_NSIS_MODIFY_PATH=ON`) — bin\ добавляется в
  системный PATH, после чего `lvrun` доступен из любой консоли;
* `CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL=ON` — перед установкой новой
  версии старая удаляется корректно (не остаётся «мусора» в реестре).

Состав пакета = результат `cmake --install`: `bin/lvrun.exe`, `bin/lvcook.exe`,
`lib/limvine.lib`, `include/limvine/**`, `share/limvine/{stdlib,templates}`,
а также SHA256-чексуммы (`CPACK_PACKAGE_CHECKSUM=SHA256`).

## Локальная сборка инсталлятора на Windows

1. Установите Visual Studio 2022 (Desktop C++), CMake ≥ 3.20 и NSIS 3.x.
2. Запустите `packaging\build_installer.bat` — он соберёт, протестирует и
   выпустит портативный ZIP.
3. Для .exe-мастера: `cpack -C build -G NSIS` (артефакт в текущем каталоге).

## Сборка через CI

```bash
git tag v0.1.0 && git push origin v0.1.0
```

Релиз появится в GitHub Releases с обоими установщиками и чексуммами.
