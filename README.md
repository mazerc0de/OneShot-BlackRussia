<div align="center">

<img src="https://i.ibb.co/Wv2YRJWT/photo-2023-10-31-16-48-34.jpg" width="650">

# OneShot Source — arm64 fork

### Чит для **BLACK RUSSIA** (`com.br.top`, версия 16.116.0 / build 1611)

<p>
<img src="https://img.shields.io/badge/Platform-Android-success?style=for-the-badge">
<img src="https://img.shields.io/badge/Language-C++-00599C?style=for-the-badge&logo=cplusplus">
<img src="https://img.shields.io/badge/Arch-arm64-blue?style=for-the-badge">
<img src="https://img.shields.io/badge/Client-16.116.0-green?style=for-the-badge">
</p>

Форк [rqce/OneShot-BlackRussia](https://github.com/rqce/OneShot-BlackRussia) — порт чита на **arm64** (оригинал был ARMv7-only).

Основная работа: оффсеты не зашиты, а **ищутся в рантайме** (signature/pattern discovery по живой памяти), поэтому часть фич переживает обновления клиента.

</div>

---

# ✨ Что работает на arm64

## 👁️ ESP

| Функция | Статус | Примечание |
| --- | :---: | --- |
| Линии (top-center) | ✅ | |
| Боксы | ✅ | |
| Никнеймы + id скинов | ✅ | `*(u32*)(ped+0x98)` |
| **Живой скелет** | ✅ | Анимированные кости: `RpHAnimHierarchy.pMatrixArray` (мировые матрицы), индекс = DFS-порядок дерева фреймов, фильтр по именам костей (убраны Finger/Toe/Jaw/Brow/breast/Belly) |
| World→Screen | ✅ | Матрица камеры ищется эвристикой в рантайме |

## 🧍 Персонаж

| Функция | Статус |
| --- | :---: |
| Бессмертие (hp pin + флаги) | ✅ |
| Бесконечная стамина | ✅ |
| Полёт | ✅ |
| Ходьба под водой | ✅ |
| Высокий/длинный прыжок | ✅ |

## 🌍 Игровой мир

| Функция | Статус |
| --- | :---: |
| Смена времени (слайдер) | ✅ | часы по `lib+0x234a0f0`: u32 hour@+4, u32 min@+8 |
| Заморозка времени | ✅ | держит час каждый кадр |

## 🔧 Прочее

- **RakNet** — хуки исходящих пакетов работают
- **Телеметрия вырезана** — `work/reporter_stub.s` заменяет `libblackhub_event_reporter.so` (Sentry-репортер, сливал бы модуль-лист с читом при краше). Клиент линкуется на неё (`DT_NEEDED`, 41 символ) — stub экспортирует всё как no-op

---

# 🚧 Не работает / в процессе

| Функция | Статус |
| --- | :---: |
| Небо/TimeCycle при смене времени | 🔶 | HUD-часы меняются, но небо читает другой источник времени (в коде лежит скан живых byte-clock структур `SKYCLK`) |
| Aimbot, NoReload, Damager, AntiAfk, BotMiner | ❌ | v7a-only, отключены `#ifndef __aarch64__` — нужны оффсеты arm64 |
| GameSpeed, длинный прыжок (глобал) | ❌ | v7a-only |
| Транспорт, тюнинг | ❌ | не портировано |

---

# 🛠️ Сборка и деплой

```bash
ndk-build                                        # собирает libs/arm64-v8a/liboneshot.so
cp libs/arm64-v8a/liboneshot.so <decoded-apk>/lib/arm64-v8a/
cp work/reporter_stub.s -> libblackhub_event_reporter.so   # собрать: clang -shared -nostdlib
apktool b <decoded-apk> -o out.apk
apksigner sign --ks debug.keystore out.apk
adb install -r out.apk
```

Инжект — подменой `liboneshot.so` внутри APK (грузится клиентом). Тестировалось на BlueStacks Air (`arm64`), adb `127.0.0.1:6610`.

---

# 📁 Структура

| Путь | Что |
| --- | --- |
| `main.cpp` | ESP-рендер, скелет, дискавери оффсетов (SKEL/CLOCK), ImGui-меню |
| `cheat/` | структуры фич, хуки |
| `game/` | обёртки CPed/CPhysical с ABI-оффсетами |
| `work/` | `reporter_stub.s` (stub Sentry-либы), `OFFSETS.md` (карта найденных оффсетов) |

---

# ⚠️ Disclaimer

Форк для образовательных целей. Оффсеты актуальны для клиента **16.116.0** — при обновлении игры часть придётся пересобрать (рантайм-дискавери часть переживёт само).
