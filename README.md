# racingcar — Luau Raycast Vehicle Physics

Biblioteca nativa de fisica veicular (C11) com interface de scripting Luau (C++) para jogos de corrida 3D. Implementa raycast suspension, mola/amortecedor, anti-roll, direcao, tracao, frenagem, pneu com aderencia lateral e atualizacao em timestep fixo.

## Projeto
- `include/rv_physics.h`: API C exportada
- `src/rv_physics.c`: dinamica veicular
- `src/rv_luau.cpp` e `src/rv_luau.h`: binding da VM Luau
- `demo/main.cpp`: simulacao com terreno plano
- `scripts/race.luau`: script configuravel sem recompilacao
- `tests/test_vehicle.c`: testes
- `CMakeLists.txt` e `build-windows.ps1`: build Visual Studio 2022

**Limite:** nao e motor de colisao 3D completo. A engine hospedeira deve fornecer raycasts da pista e colisao da carroceria.

## Windows (PowerShell)
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DRV_WITH_LUAU=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\raycast_luau_demo.exe
```

O build baixa [Luau](https://github.com/luau-lang/luau) durante a primeira configuracao, ou usa `-DLUAU_SOURCE_DIR=C:/seu/luau`.

Eixos: X direita, Y cima, Z frente; medidas SI (m, kg, N, s). Simulacao recomendada a 120 Hz.
