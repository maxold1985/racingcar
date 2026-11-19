# Luau Raycast Vehicle Physics — C11 + wrapper Luau

Biblioteca **independente de engine**, projetada para um jogo de corrida 3D com scripts interpretados em **Luau**.

- `src/rv_physics.cpp`: dinâmica de corpo rígido simplificada, raycast suspension, pneu e anti-roll em **C++17**.
- `include/rv_physics.h`: ABI C para integração com qualquer engine nativa (ou DLL no Windows).
- `include/rv_vec3.hpp`: operadores C++ para `RVVec3`, sem alterar o cabeçalho C.
- `src/rv_luau.cpp` e `src/rv_luau.h`: expõem a API `Physics.*` para scripts Luau.
- `demo/main.cpp`: executável host que compila/carrega `.luau` e fornece raycast de um plano `Y=0`.
- `scripts/race.luau`: carro 4x2 com suspensão independente, direção, aceleração e freio.
- `tests/test_vec3.cpp`: testes de operadores e compatibilidade da estrutura C.
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

São gerados `raycast_vehicle.dll`/`.lib` (API C exportada), `raycast_vehicle_static.lib` e `rv_tests.exe`/`rv_vector_tests.exe`. A DLL exporta `rv_vehicle_create`, `rv_vehicle_add_wheel`, `rv_vehicle_step` etc.

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

1. Compile e integre `raycast_vehicle` por C ou inclua os objetos estáticos C++17, preservando a ABI C.
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


## Operadores vetoriais C++17 e Luau

**C++:** `RVVec3` continua sendo um tipo `struct` com três `float`s, definido no cabeçalho público `include/rv_physics.h` para consumo de código C. A implementação da simulação passou de `src/rv_physics.c` para `src/rv_physics.cpp`. Inclua `rv_vec3.hpp` em arquivos C++ para ter:

```cpp
#include "rv_vec3.hpp"

RVVec3 a{1, 2, 3}, b{4, 5, 6};
RVVec3 soma = a + b;             // (5, 7, 9)
RVVec3 subtracao = a - b;        // (-3, -3, -3)
RVVec3 forca = a * 10.0f;       // (10, 20, 30) — substitui mul(a,10)
RVVec3 inverso = -a;            // (-1, -2, -3)
RVVec3 comp = a * b;            // (4, 10, 18) — componente a componente
RVVec3 media = (a + b) / 2.0f;
a += b;
a *= 0.5f;
float produtoEscalar = rv_math::dot(a, b);
RVVec3 produtoVetorial = rv_math::cross(a, b);
```

Operadores: `+`, `-`, negação unária, `*` (escalar e entre vetores), `/` (escalar), `+=`, `-=`, `*=`, `/=`. **Atenção:** `a * b` é multiplicação componente a componente; produto escalar é `rv_math::dot`. O cabeçalho C **não** contém sobrecarga de operadores, que só existe em C++.

**Luau:** o wrapper lê vetores nativos `vector.create(x,y,z)` *ou* tabelas tradicionais `{x=..., y=..., z=...}` como argumento. As posições, velocidades e centros/contatos de roda retornam valores **vector nativos**, aceitando operadores aritméticos:

```luau
local car = Physics.newVehicle({position = vector.create(0, 1, 0)})
-- Adicione as rodas usando Physics.addWheel(...).
local estado = Physics.getState(car)
local proximaPosicao = estado.position + estado.velocity * (1 / 120)
```

O Luau fornece `vector.dot`, `vector.cross`, `vector.normalize` e operações aritméticas nativas. Vetores Luau são imutáveis (as operações produzem um novo vetor).

**Compatibilidade:** o ABI dos exports C e o layout de `RVVec3` são preservados. Como a implementação agora é C++17, ao vincular a biblioteca **estática** de um programa C é necessário vincular também o runtime C++.

**Testes:** no ambiente Linux, `cmake -DRV_WITH_LUAU=OFF`, build e CTest passaram (2/2: `vehicle_core` e `vector_operators`). A nova integração com a VM Luau foi conferida contra a API oficial e passou na checagem sintática C++ isolada, mas **não foi executada contra a VM Luau real neste ambiente**.

Fonte da API vetorial Luau: https://luau.org/library/


## Motor gráfico 3D e leitor de modelos (raylib 5.5)

O projeto agora contém um **host gráfico 3D** opcional baseado em raylib/OpenGL 3.3, executando o **script Luau real** a cada passo de física. Os arquivos são:

- `graphics/main.cpp`: janela 3D, câmera de perseguição, teclado, loop fixo de 120 Hz, sincronização com a física e desenho das quatro rodas.
- `graphics/rv_model.hpp` e `graphics/rv_model.cpp`: leitor RAII de modelos 3D e geração de malhas substitutas quando não há arquivo.
- `scripts/racing_game.luau`: cria o carro, rodas e implementa `fixedUpdate(dt)` usando os controles fornecidos pelo host.

### Compilar e executar (Windows / Visual Studio 2022)

```powershell
git pull
.\build-windows.ps1 -Graphics
.\build\Release\racingcar_game.exe
```

Para abrir seu próprio modelo:

```powershell
.\build\Release\racingcar_game.exe "C:\meus_modelos\carro.glb"
```

Para carregar modelo e script personalizado:

```powershell
.\build\Release\racingcar_game.exe "C:\meus_modelos\carro.glb" "C:\meus_scripts\corrida.luau"
```

Se preferir dependências já clonadas:

```powershell
.\build-windows.ps1 -Graphics -LuauSourceDir "C:\src\luau" -RaylibSourceDir "C:\src\raylib"
```

**Controles:** W/S ou setas cima/baixo para acelerar/reverter; A/D ou setas esquerda/direita para direção; espaço para frear; ESC para sair. Sem arquivo de modelo, a engine gera um carro retangular e rodas cilíndricas. A pista padrão é um plano infinito Y=0.

**Formatos de modelo:** GLB, glTF, OBJ e IQM, conforme os carregadores do raylib. Para GLTF/OBJ com arquivos auxiliares, mantenha as texturas e os arquivos referenciados no caminho relativo correto. O modelo deve estar em metros, com +Y para cima e +Z para frente. O mesh da carroceria deve estar centrado no centro de massa, sem rodas incorporadas (ou elas aparecerão duplicadas). As rodas são geradas pela engine e animadas separadamente.

**Escopo atual:** o leitor importa geometria e materiais para **renderização**, não constrói automaticamente colisores físicos a partir do modelo. O raycast do exemplo ainda é feito sobre o plano Y=0. A biblioteca não inclui editor visual, animação esquelética, carregamento de cenas completas, sombras dinâmicas ou colisões entre veículos. Para circuitos reais, substitua `ground_ray()` por raycast em BVH/malha de colisão da pista.

**Validação:** os testes do núcleo C/C++ continuam disponíveis via `ctest`; a compilação e execução do novo alvo gráfico com dependências reais ainda devem ser verificadas no ambiente Windows do usuário. Dependências externas são obtidas pelo CMake FetchContent, e nenhuma delas foi copiada para o repositório.


### Diagnóstico: raylib renderiza a 120 FPS, mas o carro não acelera

A falha foi localizada na integração C++/Luau. O host reconstruía o global `Input` a cada passo físico, enquanto a otimização `GETIMPORT` do Luau pode armazenar em cache cadeias globais (`Input.throttle`). O resultado pode ser o script sempre receber `throttle=0`.

**Correções aplicadas:** o host atualiza os campos da mesma tabela `Input`, o compilador recebe `mutableGlobals={"Input"}`, e o script mantém uma referência `local input = Input`. O painel gráfico mostra tanto `INPUT` (teclado da raylib) quanto `LUAU` (valores realmente recebidos por `Physics.setControls()`), além de `VM physics steps` e contatos das rodas.

Após compilar com `RV_WITH_LUAU=ON` execute a regressão sem abrir janela:

```powershell
ctest --test-dir build -C Release -R luau_live_input --output-on-failure
```

O teste `tests/test_luau_input.cpp` usa a VM Luau real e o script `scripts/racing_game.luau` e verifica a transição de acelerador 0→1→0, frenagem, direção e deslocamento físico. A compilação e execução desse novo teste precisam ser realizadas em uma máquina com as bibliotecas Luau reais; não foram executadas neste ambiente.

Observação: `Indices data converted from u32 to u16` é um aviso separado do carregador GLB da raylib e pode causar defeitos na malha, mas não é evidência de bloqueio do teclado.

Referências de suporte: https://luau.org/api/ e https://github.com/luau-lang/luau/blob/master/Compiler/include/luacode.h


### Câmera orbital e direção invertida

- **Botão direito do mouse + arrastar:** orbitar ao redor do veículo (yaw e pitch, com limite vertical).
- **Roda do mouse:** zoom de 3 a 35 metros.
- **Home:** restaurar ângulo e distância da câmera.
- **W/S:** acelerar / marcha à ré; **espaço:** frear.
- **A/seta esquerda:** direção invertida (+1); **D/seta direita:** direção invertida (-1).
- **R:** reiniciar posição e velocidade do veículo.

A câmera acompanha a posição do carro com suavização exponencial, mas usa orientação vertical global para evitar que capotamentos girem a câmera. A alteração de direção foi feita na leitura de teclado em `graphics/main.cpp`; a API física `Physics.setControls` mantém sua convenção original.


### Materiais e texturas GLB/glTF

O leitor `RVModel::load()` usa `LoadModel()` do raylib 5.5, que já importa malhas, materiais e texturas suportados do arquivo GLB/glTF. O jogo agora renderiza o chassi com `WHITE`, preservando as cores e texturas importadas; antes aplicava um tint vermelho a todos os materiais. O leitor mantém `model.materials` e `model.meshMaterial` sem substituí-los e registra no console os materiais e IDs das texturas de albedo, normal, metallic, roughness e emissão.

Para GLB, as texturas normalmente ficam incorporadas no arquivo; para glTF com arquivos externos, mantenha os recursos relativos próximos ao arquivo principal. O renderer padrão do raylib **não é um pipeline PBR completo**: mapas normal/metallic/roughness podem ser importados mas exigem shaders próprios para que seus efeitos sejam efetivamente renderizados. A implementação atual preserva o material e a textura base (albedo), sem prometer iluminação PBR, transparência avançada ou suporte a todas as extensões glTF.

Atualize e recompile: `git pull origin mainn` e `cmake --build build --config Release --parallel`.


### Textura WebP do pavimento

O jogo carrega automaticamente `..\\..\\assets\\pavimento.webp` quando executado a partir de `build\\Release`. A imagem é decodificada pela biblioteca **libwebp** (raylib 5.5 não decodifica WebP nativamente), enviada à GPU como textura RGBA e repetida a cada 5 metros no chão de 1500 × 1500 metros. O raycast continua usando Y=0.

```powershell
git pull origin mainn
.\\build-windows.ps1 -Graphics
cd build\\Release
.\\racingcar_game "..\\..\\assets\\car retro.glb"
```

O terceiro argumento permite alterar a textura: `racingcar_game.exe "car.glb" "script.luau" "pavimento.webp"`. Para compilar offline com libwebp já baixada, configure `-DWEBP_SOURCE_DIR=C:/src/libwebp`. Se a imagem não existir ou for inválida, o jogo usa o chão verde original.


### Personagem a pé e entrada no carro

O jogo inicia no modo **a pé**. O componente `graphics/rv_player.hpp/.cpp` controla um personagem sobre o terreno plano com WASD e orientação relativa à câmera. Por padrão desenha um personagem geométrico simples. Opcionalmente, carregue um GLB/OBJ/glTF/IQM de personagem como **quarto argumento** (após carro, script e textura):

```powershell
.\\racingcar_game "..\\..\\assets\\car retro.glb" "..\\..\\scripts\\racing_game.luau" "..\\..\\assets\\pavimento.webp" "..\\..\\assets\\personagem.glb"
```

Um **ghost azul** marca a porta do motorista. Aproxime-se (até 1,7 m) e pressione **ESPAÇO** para entrar. Dentro do carro, WASD controla a direção/aceleração, ESPAÇO freia e **F** sai. Fora do carro, a simulação continua com freio aplicado. A câmera acompanha o personagem a pé e o veículo quando embarcado, preservando órbita e zoom.

O ponto da porta está em coordenadas locais do carro: `doorLocal{-1.05f, 0.0f, 0.35f}` em `graphics/main.cpp`; ajuste-o ao GLB utilizado. O componente atual usa movimento cinemático sobre Y=0, sem colisões de cápsula, animação esquelética ou detecção de paredes. O modelo opcional é estático (sem playback de animação). 


### Animações Mixamo FBX (Assimp)

O componente `graphics/rv_animation.hpp/.cpp` importa personagem **FBX ou GLB com ossos** usando Assimp, carrega clips FBX separados e aplica deformação por skinning na CPU. `RVPlayer` seleciona automaticamente `Idle` (parado) e `Walking` (movendo). API:

```cpp
player.load("personagem.fbx");                  // malha + esqueleto
player.loadAnimation("Idle", "Idle.fbx");
player.loadAnimation("Walking", "Walking.fbx");
player.loadAnimation("EnterCar", "EnterCar.fbx");
player.playAnimation("Walking", true);          // loop
player.playAnimation("EnterCar", false);        // uma vez; finished() sinaliza fim no componente RVAnimation
```

Argumentos do executável: **carro, script Luau, pavimento, personagem, Idle, Walking, EnterCar**:

```powershell
.\\racingcar_game "..\\..\\assets\\car retro.glb" "..\\..\\scripts\\racing_game.luau" "..\\..\\assets\\pavimento.webp" "..\\..\\assets\\personagem.fbx" "..\\..\\assets\\Idle.fbx" "..\\..\\assets\\Walking.fbx" "..\\..\\assets\\EnterCar.fbx"
```

Os arquivos devem usar **o mesmo esqueleto e os mesmos nomes de ossos**; no Mixamo, exporte o personagem **With Skin** e os clips adicionais **Without Skin**, sem alterar o rig. O carregamento de EnterCar está disponível, mas a entrada no carro ainda é instantânea; a reprodução visual da transição depende de conectar uma máquina de estados de embarque. Não há blend entre clips, root motion, colisão de cápsula ou suporte a todos os materiais/texturas FBX (o renderer de skinning atual usa material padrão). A importação via Assimp aumenta o tempo de configuração/compilação; também é possível apontar `ASSIMP_SOURCE_DIR` para uma cópia local.
