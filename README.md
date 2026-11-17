# Luau Raycast Vehicle Physics — C11 + wrapper Luau

Biblioteca **independente de engine**, projetada para um jogo de corrida 3D com scripts interpretados em **Luau**.

- `src/rv_physics.c`: dinâmica de corpo rígido simplificada, raycast suspension, pneu e anti-roll em **C11**.
- `include/rv_physics.h`: ABI C para integração com qualquer engine nativa (ou DLL no Windows).
- `src/rv_luau.cpp` e `src/rv_luau.h`: expõem a API `Physics.*` para scripts Luau.
- `demo/main.cpp`: executável host que compila/carrega `.luau` e fornece raycast de um plano `Y=0`.
- `scripts/race.luau`: carro 4x2 com suspensão independente, direção, aceleração e freio.
- `tests/test_vehicle.c`: testes com terreno plano, suspensão, aceleração, freio e parâmetros de tempo inválidos.

**Limite importante:** esta é uma **biblioteca de dinâmica veicular**, não um motor de colisão 3D. O jogo deve fornecer raycasts contra a pista, colisões do chassi com o mundo, obstáculos, barreiras, contatos entre carros, renderização e áudio. Sem colisão adicional do chassi, capotamentos/penetrações extremas não são resolvidos.

## Compilar no Windows (VS 2022 / PowerShell)

Instalar Git, CMake e Visual Studio 2022 com **Desenvolvimento para desktop com C++**. Dentro da pasta do projeto:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DRV_WITH_LUAU=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\raycast_luau_demo.exe
```

Na primeira configuração CMake usa `FetchContent` para obter o Luau do repositório oficial (`https://github.com/luau-lang/luau`). Requer conexão com a internet.

Se você já clonou o Luau localmente, evite baixar de novo:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DRV_WITH_LUAU=ON -DLUAU_SOURCE_DIR="C:/codigo/luau"
cmake --build build --config Release --parallel
```

Para executar outro script:

```powershell
.\build\Release\raycast_luau_demo.exe "C:\meu_jogo\scripts\pista01.luau"
```

O exemplo sempre carrega `scripts/race.luau` do diretório-fonte quando o caminho não é passado. Você pode substituir o arquivo sem recompilar o executável.

## Compilar apenas a biblioteca C (sem Luau)

```powershell
cmake -S . -B build-c -G "Visual Studio 17 2022" -A x64 -DRV_WITH_LUAU=OFF
cmake --build build-c --config Release
ctest --test-dir build-c -C Release --output-on-failure
```

São gerados `raycast_vehicle.dll`/`.lib` (API C exportada), `raycast_vehicle_static.lib` e `rv_tests.exe`. A DLL exporta `rv_vehicle_create`, `rv_vehicle_add_wheel`, `rv_vehicle_step` etc.

No Linux:

```sh
cmake -S . -B build -G Ninja -DRV_WITH_LUAU=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Modelo físico

Coordenadas da biblioteca: **+X direita, +Y cima, +Z frente**. Todas as grandezas usam **SI**: metros, kg, segundos, newtons e radianos. Não são as convenções do `CFrame` do Roblox, que usa tipicamente `-Z` como direção à frente.

O host fornece a função `RVRaycastFn`, que recebe a origem da suspensão, direção e comprimento máximo. Cada roda usa:

- `ray_length = maxLength + radius`
- `length = clamp(hit.distance - radius, minLength, maxLength)`
- `compression = max(0, restLength - length)`
- `F_susp = clamp(k * compression - c * dot(v_anchor - v_ground, up), 0, maxSuspensionForce)`
- `F_grip_limit = friction * F_normal`

Usa velocidade do **ponto do chassi no ancoramento** (`v + ω × r`) para amortecer, sem derivar numericamente diferenças de comprimento entre quadros. A força de suspensão é aplicada no ancoramento. A tração e a força lateral dos pneus são aplicadas no contato com o solo, gerando arfagem/rolagem; força combinada longitudinal+lateral limitada por um círculo de atrito. Cada par do mesmo eixo recebe barra anti-roll independente.

Integração do corpo: velocidade/posição por Euler semimplícito, velocidade angular com tensor de inércia diagonal no espaço local e quaternion normalizado para orientação. A gravidade é aplicada internamente: não aplique uma segunda vez no host.

**Atualização recomendada: 120 Hz fixos** (`1/120`). Faça acumulação de tempo no loop da sua engine. `rv_vehicle_step` rejeita valores inválidos ou superiores a `1/30` s, para reduzir instabilidade numérica. O renderizador pode interpolar poses de física.

A estabilidade é calibrada para um carro de ~1200 kg com **4 rodas** e pista plana: isto é um protótipo de física de corrida, não um solver de contatos completo, nem um modelo de pneus Pacejka.

## Interface interpretada Luau

| Função | Descrição |
|---|---|
| `Physics.newVehicle(config)` | Cria veículo; retorna ID inteiro |
| `Physics.addWheel(id, config)` | Adiciona roda; retorna índice (começa em 1) |
| `Physics.setPose(id, position, quaternion)` | Define posição/orientação |
| `Physics.setVelocity(id, linear, angular)` | Define velocidades |
| `Physics.setControls(id, throttle, brake, steer)` | Controles: `[-1,1]`, `[0,1]`, `[-1,1]` |
| `Physics.step(id, fixedDt)` | Simula um passo fixo usando raycast do host |
| `Physics.getState(id)` | `position`, `orientation`, `velocity`, `angularVelocity`, `speedKmh` |
| `Physics.getWheel(id, index)` | `grounded`, `force`, `compression`, `length`, `center`, `spinAngle`, `steerAngle` e velocidades |
| `Physics.destroy(id)` | Libera o veículo |

Exemplo resumido:

```luau
local car = Physics.newVehicle({mass = 1200, position = {x=0, y=0.98, z=0}})
Physics.addWheel(car, {mount={x=-0.8,y=-0.2,z=1.3}, steering=true, axle=0})
-- Adicione as outras 3 rodas, veja scripts/race.luau.
function fixedUpdate(dt)
    Physics.setControls(car, 1, 0, 0)
    Physics.step(car, dt)
    local state = Physics.getState(car)
    -- A engine usa state.position / state.orientation para atualizar o mesh 3D.
end
```

### Opções `Physics.newVehicle(config)`

`mass`, `inertia={x,y,z}`, `position`, `orientation={x,y,z,w}`, `gravity`, `linearDrag`, `angularDrag`, `maxSteer` (radianos), `engineForce` (N), `brakeForce` (N), `antirollFront` e `antirollRear` (N/m). Valores omitidos usam `rv_default_body()`.

### Opções `Physics.addWheel(id, config)`

`mount={x,y,z}`, `radius`, `restLength`, `minLength`, `maxLength`, `spring`, `bumpDamping`, `reboundDamping`, `maxSuspensionForce`, `friction`, `lateralStiffness`, `rollingResistance`, `steering` (booleano), `driven` (booleano), `axle` (`0` frente / `1` traseira). Valores omitidos usam `rv_default_wheel()`.

### Integrar com um motor 3D próprio

1. Compile e integre `raycast_vehicle` por C ou inclua os objetos estáticos C11.
2. Registre `rv_register_luau(lua_State* L, RVLuauContext* context)` antes de ativar o sandbox Luau.
3. Implemente `context.raycast` como consulta ao BVH, Bullet, Jolt ou colisor da pista, retornando distância, ponto, normal e velocidade da superfície.
4. Carregue o `.luau` usando `luau_compile()` e `luau_load()` e execute `fixedUpdate(1/120)` em thread sandboxed.
5. Atualize carroceria e malhas das rodas a partir de `Physics.getState()` e `Physics.getWheel()`. `spinAngle` é visual, não uma simulação do torque/inércia rotacional da roda.

**Segurança:** scripts não confiáveis devem ter sandbox isolado, restrições de CPU/memória e host API cuidadosamente limitada; este exemplo ativa o sandbox do Luau, mas não estabelece cotas de recursos. O wrapper usa IDs em vez de expor ponteiros do host aos scripts.

## Base técnica

- [Luau — repositório oficial](https://github.com/luau-lang/luau)
- [Luau — API C de incorporação](https://luau.org/api/)
- [Bullet — referência de implementação de veículo por raycast](https://github.com/bulletphysics/bullet3/blob/master/src/BulletDynamics/Vehicle/btRaycastVehicle.cpp)

O código nesta pasta é uma implementação original de demonstração, não contém trechos copiados do Bullet. Luau é obtido separadamente e possui licença MIT.
