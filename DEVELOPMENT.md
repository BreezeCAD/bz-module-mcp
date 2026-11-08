# Разработка модуля MCP

Модуль живёт в своём репозитории (раньше — `modules/mcp` в BreezeCAD/breezecad; история перенесена). Устройство — как в [шаблоне модулей](https://github.com/BreezeCAD/bz-module-template).

| Папка | Что |
|---|---|
| `src/` | сервер (`McpModule.cpp`, `Net`, `Connection`, `Protocol`) и мост `Bridge.cpp` |
| `sdk/bz_module.h` | C-интерфейс модулей — копия из BreezeCAD (`sdk/SOURCE`) |
| `vendor/platform/Paths.*` | поиск папки настроек — копия из BreezeCAD, нужна мосту (`vendor/SOURCE`) |
| `third_party/` | nlohmann/json, doctest |
| `tests/` | протокол; настоящий модуль с поддельной программой (сервер, главный поток, выключение); состав пакета |

## Собрать и проверить
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Пакет — `build/packages/bz-mcp-<версия>-<система>-<архитектура>.bzmod`. Тесты занимают порт 47399 на 127.0.0.1.

## Выпуск
Поднять `version` в `module.json` → в `main` → на GitHub **Releases → Draft a new release**, тег `vX.Y.Z` той же версии → **Publish**. GitHub соберёт Windows (MSYS2 MinGW, как BreezeCAD), Linux (glibc 2.28) и macOS (Apple Silicon), прогонит тесты и приложит к выпуску три `.bzmod`.

## Обновить копии из BreezeCAD
`scripts/update-sdk.sh` (из `main`) или `scripts/update-sdk.sh v0.2.6` (из выпуска) — обновляет `bz_module.h` и `Paths.*`.
