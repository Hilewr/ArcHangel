# ArcHangel | Alpha

**Русский** · [English](#english)

Оверлей, рекордер забегов и набор инструментов движения для **Half-Life 1 / GoldSrc (OpenGL)** в **одиночной игре**.
Внутренний (internal) модуль на C++: загрузчик подгружает DLL в `hl.exe`, DLL рисует меню на Dear ImGui и подключается к игре через перехват функций (MinHook). Также проверялся в Counter-Strike 1.6 в офлайне.

> **Статус: альфа.** Часть функций проверена не на всех картах и версиях игры. Если что-то не работает, откройте issue и приложите `hltas.log`.

## Важное предупреждение

- Проект **только для одиночной игры и офлайн-режима** (в том числе игра против ботов на своём компьютере).
- Не заходите с ним на серверы с античитом. Подмена кода в процессе игры может привести к блокировке (VAC). Запускайте игру с параметром `-insecure`.
- Проект не связан с Valve. Игровые данные (карты, модели) только читаются и не изменяются.
- Приёмы, рассчитанные на игру по сети, в проект не входят и добавляться не будут.

## Возможности

| Раздел меню | Что умеет |
|---|---|
| **Info** | Данные игрока (скорость, позиция, земля/воздух), статус перехватов, FPS и стоимость оверлея |
| **Time** | Замедление, пауза, шаг по кадрам, плавное замедление, FPS booster (Engine / Cmd) |
| **Movement** | Автобанихоп, **автострафе** (режимы Auto / Helper, выбор направления: вперёд, влево, вправо, назад или по нажатым W/A/S/D, ползунок мощности **Power**), граунд-страйф, статистика прыжков, путь в мире |
| **Recorder** | Запись забегов, список, виды Orbit / Top / POV, график скорости, статистика, Trim / Save / Delete, карта (BSP), воспроизведение (в том числе Lock-Step) |
| **Models** | Подсветка 3D-моделей и оружия (Tint / Flat / Glow, цвет, яркость, радуга, «поверх стен»), 3D-превью настоящих моделей `.mdl` |
| **View** | Поле зрения мира, поле зрения и положение оружия, растяжение картинки, убрать визуальную отдачу, вид от третьего лица |
| **Keys** | Назначение клавиш |
| **Settings** | Тема (Neon / Mono), совместимость с программами записи экрана, отключение VSync, сохранение настроек |

### Клавиши по умолчанию (меняются в разделе Keys)

| Клавиша | Действие |
|---|---|
| `Insert` | открыть / закрыть меню |
| `F6` | пауза |
| `F7` | шаг на один кадр |
| `F8` | замедление вкл / выкл |
| `F9` | автобанихоп |
| `F10` | автострафе |
| `G` (удерживать) | граунд-страйф |
| `F11` | запись забега |
| `F12` | воспроизведение / остановка |

> В Half-Life `F6` и `F7` — быстрое сохранение и загрузка. Здесь они заняты паузой и шагом: переназначьте их в Keys, если нужны.

## Сборка (Windows, Visual Studio)

Нужны Visual Studio 2022 или новее с рабочей нагрузкой «Разработка классических приложений на C++».

1. Откройте `HLTas.sln`.
2. Выберите **Release** и **x86** (игра 32-битная, с x64 DLL не загрузится).
3. Соберите решение (`Ctrl+Shift+B`).
4. В папке `bin` появятся `HLTas.dll` и `HLTasLoader.exe`. Они должны лежать вместе.

Если проект лежит в папке OneDrive и сборка падает с «не удаётся открыть файл HLTas.dll», закройте игру или перенесите проект, например, в `C:\HLTas`.

## Запуск

1. В Steam: Half-Life → Свойства → Параметры запуска: `-insecure`.
2. Запустите `HLTasLoader.exe` (он ждёт игру).
3. Запустите Half-Life и загрузите карту.
4. Загрузчик сам подгрузит DLL. В игре нажмите `Insert`.

В игре должен быть выбран рендерер **OpenGL** (Options → Video → Renderer).

### Файлы, которые создаёт программа

| Файл / папка | Где | Что |
|---|---|---|
| `hltas.ini` | рядом с `hl.exe` | настройки и клавиши |
| `hltas.log` | рабочая папка игры | журнал: какие перехваты встали, определение карты, итоги воспроизведения |
| `archangel_runs\*.run` | рядом с `hl.exe` | записанные забеги |

## Заметки по использованию

- **Поле зрения.** Широкий FOV включается командой движка `default_fov`, поэтому на краях экрана нет пустоты. Прежнее значение возвращается при выключении функции.
- **Запись экрана (OBS, Discord).** Если при записи чёрный экран или зависания, откройте Settings → Compatibility и переключите «Capture-friendly hook» и «Hook GDI SwapBuffers».
- **Автострафе.** Мышью поворачиваете вы, страфы подстраиваются под поворот. Ползунок **Power** меняет силу команды движения; на 100 % используется лучший угол для текущего `sv_airaccelerate`.
- **Воспроизведение.** `F11` начинает и заканчивает запись, `F12` запускает повтор. Запись, сделанная на другой карте, не воспроизводится. Показатель `drift` на экране — расхождение с записью.

## Лицензии

Dear ImGui (MIT) и MinHook (BSD-2-Clause) лежат в `HLTas/third_party` вместе со своими лицензиями.

---

<a id="english"></a>

# ArcHangel | Alpha

[Русский](#archangel--alpha) · **English**

An overlay, run recorder and movement toolkit for **Half-Life 1 / GoldSrc (OpenGL)**, intended for **single-player** use.
It is an internal C++ module: a loader injects the DLL into `hl.exe`, and the DLL draws a Dear ImGui menu and attaches to the game through function hooks (MinHook). Also tested in Counter-Strike 1.6 offline.

> **Status: alpha.** Some features have not been verified on every map and game build. If something does not work, open an issue and attach `hltas.log`.

## Important notice

- **Single-player and offline only** (including bot matches on your own machine).
- Do not join servers with anti-cheat. Modifying game code at runtime can get you banned (VAC). Launch the game with `-insecure`.
- Not affiliated with Valve. Game data (maps, models) is only read, never modified.
- Techniques designed for network play are not part of this project and will not be added.

## Features

| Menu page | What it does |
|---|---|
| **Info** | Player data (speed, position, ground/air), hook status, FPS and overlay cost |
| **Time** | Slow motion, pause, frame step, smooth slow motion, FPS booster (Engine / Cmd) |
| **Movement** | Auto bhop, **auto strafe** (Auto / Helper modes; direction: forward, left, right, back, or follow the held W/A/S/D keys; **Power** slider), ground strafe, jump statistics, in-world path |
| **Recorder** | Run recording, run list, Orbit / Top / POV views, speed graph, statistics, Trim / Save / Delete, BSP map view, replay (including Lock-Step) |
| **Models** | Highlight 3D models and weapons (Tint / Flat / Glow, colour, brightness, rainbow, through walls), 3D preview of real `.mdl` models |
| **View** | World FOV, viewmodel FOV and offset, image stretch, remove visual recoil, third-person view |
| **Keys** | Key bindings |
| **Settings** | Theme (Neon / Mono), screen-capture compatibility, VSync off, saving settings |

### Default keys (rebindable on the Keys page)

| Key | Action |
|---|---|
| `Insert` | open / close the menu |
| `F6` | pause |
| `F7` | single frame step |
| `F8` | slow motion on / off |
| `F9` | auto bhop |
| `F10` | auto strafe |
| `G` (hold) | ground strafe |
| `F11` | start / stop recording |
| `F12` | play / stop replay |

> In Half-Life `F6` and `F7` are quick save and quick load. Here they are used for pause and step: rebind them on the Keys page if you need them.

## Building (Windows, Visual Studio)

Requires Visual Studio 2022 or newer with the "Desktop development with C++" workload.

1. Open `HLTas.sln`.
2. Select **Release** and **x86** (the game is 32-bit; an x64 DLL will not load).
3. Build the solution (`Ctrl+Shift+B`).
4. `HLTas.dll` and `HLTasLoader.exe` appear in `bin`. Keep them together.

If the project lives in a OneDrive folder and the build fails with "cannot open file HLTas.dll", close the game or move the project, for example to `C:\HLTas`.

## Running

1. In Steam: Half-Life → Properties → Launch options: `-insecure`.
2. Start `HLTasLoader.exe` (it waits for the game).
3. Start Half-Life and load a map.
4. The loader injects the DLL by itself. Press `Insert` in game.

The game must use the **OpenGL** renderer (Options → Video → Renderer).

### Files created by the program

| File / folder | Location | Contents |
|---|---|---|
| `hltas.ini` | next to `hl.exe` | settings and key bindings |
| `hltas.log` | game working directory | log: installed hooks, map detection, replay results |
| `archangel_runs\*.run` | next to `hl.exe` | recorded runs |

## Usage notes

- **Field of view.** A wide FOV is applied through the engine command `default_fov`, so there are no empty areas at the screen edges. The original value is restored when the feature is turned off.
- **Screen capture (OBS, Discord).** If recording shows a black screen or freezes, go to Settings → Compatibility and toggle "Capture-friendly hook" and "Hook GDI SwapBuffers".
- **Auto strafe.** You turn with the mouse, the strafes follow your turning. The **Power** slider sets the strength of the movement command; at 100 % the best angle for the current `sv_airaccelerate` is used.
- **Replay.** `F11` starts and stops recording, `F12` starts a replay. A run recorded on a different map will not play. The on-screen `drift` value shows the deviation from the recording.

## Licences

Dear ImGui (MIT) and MinHook (BSD-2-Clause) are included in `HLTas/third_party` together with their licences.