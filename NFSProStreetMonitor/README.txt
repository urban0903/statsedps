================================================================================
  NFS ProStreet Monitor — Нативное приложение (Win32 x86 EXE + Hook DLL)
================================================================================

ОСОБЕННОСТИ НАТИВНОЙ ВЕРСИИ:
  • Не требует Cheat Engine и не требует ASI Loader.
  • Нагрузка на ЦП < 0.5% (никаких Lua-циклов и тяжёлых MemScan).
  • Внутрипроцессный сканер Blueprint (~10 мс) + взвешенное VLT-голосование по
    всем 150 слотам: находит Blueprint активной машины для всех 76 авто сразу,
    без захода в Autosculpt (включая 40 авто, у которых bpCarType = nil).
  • Кнопки в верхней панели: [Найти BP], [◄ BP], [BP ►], [Автовыбор] — можно
    мгновенно переключаться между несколькими Blueprint одной модели (например,
    2 конфигурации в Гараже или игрок vs бот той же модели на Race Day).
  • Читает твои существующие файлы cars_stock_db.lua и parts_profiles_db.lua
    напрямую (конвертировать базы не нужно).
  • При закрытии окна EXE автоматически восстанавливает все 7 перехватов и
    чисто выгружает prostreet_hooks.dll из nfs.exe (игра продолжает работать).

СОСТАВ ПРОЕКТА:
  common/shared_ipc.h       — общий формат Shared Memory и таблица 30 ползунков
  common/lua_table_parser.h — встроенный C++ парсер .lua баз (0 зависимостей)
  common/brand_db.h         — расшифровка брендов, этапов и алиасов 76 машин
  hook_dll/prostreet_hooks.cpp — 32-bit DLL (7 перехватов + VLT + BP-сканер)
  app_exe/db_and_ipc.h      — загрузка баз, поиск nfs.exe, инжектор DLL, IPC
  app_exe/main_app.cpp      — Win32 окно (двойная буферизация GDI, 4 вкладки)
  build_msvc.bat            — сборка в один клик через Visual Studio (MSVC x86)
  build_mingw.bat           — сборка в один клик через MinGW-w64 (i686 32-bit)
  CMakeLists.txt            — альтернативная сборка через CMake

КАК СОБРАТЬ:
  Вариант A (Visual Studio — самый простой):
    1. Установи Visual Studio 2022 Community или Build Tools (компонент
       "Разработка классических приложений на C++").
    2. Запусти build_msvc.bat — скрипт сам найдёт Visual Studio и соберёт:
         bin\NFSProStreetMonitor.exe
         bin\prostreet_hooks.dll

  Вариант B (MinGW-w64 32-bit / MSYS2 mingw32):
    1. Установи MSYS2 и пакет mingw-w64-i686-gcc (или сборку winlibs i686).
    2. Запусти build_mingw.bat — соберёт те же 2 файла в папку bin\.

  ВАЖНО: Оба файла (EXE и DLL) должны быть 32-битными (x86), так как nfs.exe —
  32-битная игра. Скрипты build_msvc.bat и build_mingw.bat уже настроены на x86.

КАК ЗАПУСКАТЬ:
  1. Положи bin\NFSProStreetMonitor.exe и bin\prostreet_hooks.dll в одну папку
     (можно туда же положить cars_stock_db.lua и parts_profiles_db.lua, либо
     выбрать их один раз кнопками «Файл базы стока...» / «Файл VLT-базы...»).
  2. Запусти NFS ProStreet (nfs.exe).
  3. Запусти NFSProStreetMonitor.exe (если игра запущена от администратора —
     запусти монитор тоже от имени администратора).
  4. Программа сама найдёт nfs.exe, внедрит prostreet_hooks.dll и начнёт
     отображать телеметрию, ползунки, Autosculpt, Совпадения и Data Info.
