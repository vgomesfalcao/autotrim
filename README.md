# AutoTrim

Plugin de áudio (C++ / [JUCE 8](https://juce.com)) que regula automaticamente o ganho (trim) de cada canal com base no pico medido, com um **painel de controle** central que mede e regula todos os canais de uma vez.

## Como funciona

- Insira o **AutoTrim** em todos os canais da sessão.
- Em cada instância (modo canal): defina o **nome** do canal e o **target** de pico (dBFS), veja o trim aplicado e ligue/desligue a **automação** e o **rider** (modo contínuo).
- Em uma instância qualquer (ex.: no master), marque **"Usar esta instância como painel de controle"**: ela lista todos os canais com o nível de entrada (pré-trim) ao vivo, o trim atual e o toggle de automação de cada um.
- No painel, configure a **duração da medição** (padrão 10 s) e o **limite de trim ±dB** (padrão ±24) e clique em **"Medir e regular todos os canais"**. Ao fim da janela, cada canal recebe `trim = target − pico medido` (limitado ao ±limite).
- Canal com pico abaixo de **-60 dBFS** durante a medição é marcado **"sem sinal"** e ignorado.
- O **rider** (modo contínuo, por canal) mantém o nível no target com **perfis por tipo de fonte** — o preset pré-seleciona o perfil, mas você pode trocá-lo livremente no dropdown "Perfil do rider":

| Perfil | Detector | Sobe / Desce | Ride | Sensibilidade padrão | Retorno em pausa |
|---|---|---|---|---|---|
| **Voz** | pico com hold deslizante de 2.5 s | 2 / 6 dB/s | ±6 dB | -45 dBFS | 2 dB/s |
| **Instrumento** | pico com hold deslizante de 3 s | 1.5 / 6 dB/s | ±4 dB | -50 dBFS | 1 dB/s |
| **Bateria** | média dos picos das últimas 8 batidas (janela 50 ms, retrigger 100 ms) | 1.5 / 4 dB/s | ±4 dB | -40 dBFS | 0.5 dB/s |

  - O ride é um **offset limitado em volta do trim medido** (não mexe no parâmetro Trim) — segurança contra o ganho fugir do calibrado.
  - **Sensibilidade** (por canal): abaixo dela o rider não persegue ruído/vazamento; o offset desliza de volta ao trim medido. Trocar o perfil restaura a sensibilidade padrão dele.
  - Bateria é por batida (estilo Drum Leveler): rider contínuo bombearia entre os hits.
  - Zona morta de ±1 dB em volta do target: trim correto fica intocado.

- **Proteção contra overload** (sempre ativa com a automação ligada, mesmo sem rider): se a saída passar de `target + 3 dB` **5 vezes em 3 segundos** (overload contínuo conta 1 evento a cada 300 ms), o plugin corta automaticamente um offset de trim para trazer o pior pico de volta ao target (corte máximo −12 dB). O ajuste aparece **em vermelho** (badge `PROT` no canal, status no painel, nome em vermelho no modo compacto). Uma nova medição zera a proteção.

Target, trim, automação e rider são **parâmetros do host** (automatizáveis e salvos na sessão); nome, modo painel e as configurações globais do painel também são salvos.

## Build no Linux (via Docker)

Requer apenas Docker; tudo roda no container:

```sh
scripts/docker-build.sh          # configura + compila (VST3 + testes)
scripts/docker-build.sh test     # compila e roda os testes de DSP
```

O VST3 sai em `build/AutoTrim_artefacts/Release/VST3/AutoTrim.vst3`. Para testar no Reaper/Bitwig, copie para `~/.vst3/`.

## Build no macOS (AU + VST3 para o Logic)

O JUCE gera **Audio Unit nativamente** — sem wrapper. No Mac (Xcode CLT + CMake):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release
```

Os bundles saem em `build/AutoTrim_artefacts/Release/`:
- `AU/AutoTrim.component` → copie para `~/Library/Audio/Plug-Ins/Components/`
- `VST3/AutoTrim.vst3` → `~/Library/Audio/Plug-Ins/VST3/`

Valide com `auval -v aufx Atrm Vgfa` e abra o Logic (se não aparecer, reinicie o Logic ou rode `killall -9 AudioComponentRegistrar`).

## Roadmap

- **Sidechain de música (estilo Vocal Rider)**: subir a voz automaticamente quando a banda cresce, alimentando o rider com um sidechain do mix. Pesquisado e especificado, deixado para depois por complexidade × retorno (ver Waves Vocal Rider: parâmetro "Music Sensitivity").
- Aproximação de "Spill" (rejeição de vazamento) mais inteligente que o threshold de sensibilidade, se necessário na prática.

## Limitações conhecidas

- O painel enxerga apenas instâncias carregadas **no mesmo processo** do host. Funciona no Logic (AUv2 in-process, padrão), Reaper, Bitwig etc.; não funciona se o host isolar plugins em processos separados (sandbox AUv3, "separate process" do Bitwig).
- Waves eMotion LV1 não aceita plugins de terceiros; para uso ao vivo, uma opção é o Waves SuperRack Performer (VST3).

## Estrutura

- `src/PluginProcessor.*` — AudioProcessor, parâmetros, `processBlock()` (metering, trim suavizado, medição, rider)
- `src/Registry.*` — registro global de instâncias + configurações do painel
- `src/Dsp.h` — matemática de ganho/rider/envelope (pura, testável sem JUCE)
- `src/Measurement.*` — orquestração da medição em massa
- `src/PluginEditor.*`, `src/LookAndFeel.h` — GUI (modo canal e modo painel)
- `tests/DspTests.cpp` — testes unitários do DSP
- `docker/`, `scripts/docker-build.sh` — build containerizado
- `archive/rust-nih-plug/` — versão anterior em Rust (arquivada)
