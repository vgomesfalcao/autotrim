# AutoTrim

Plugin de áudio (C++ / [JUCE 8](https://juce.com)) que regula automaticamente o ganho (trim) de cada canal com base no nível medido, com um **painel de controle** central que mede e regula todos os canais de uma vez.

## Visão geral

O AutoTrim tem dois modos de operação numa mesma instância:

- **Modo canal** — inserido em uma track, mede o sinal e aplica o trim daquele canal, com automações ao vivo opcionais (rider, AGC, Clip Guard).
- **Modo painel de controle** — uma instância qualquer (ex.: no master) marcada como painel lista todos os canais e mede/regula todos de uma vez. Nesse modo a instância é **só gerenciamento**: o áudio passa intocado por ela (sem ganho, rider ou proteção).

Target, trim, automação, rider, AGC e Clip Guard são **parâmetros do host** (automatizáveis e salvos na sessão); nome, modo painel e as configurações globais do painel também são salvos.

## Instalação nos canais

- Insira o **AutoTrim** em todos os canais da sessão.
- Em cada instância (modo canal), defina o **nome** do canal e o **Target** de pico (dBFS), veja o trim aplicado e ligue/desligue a **Automação** e o **Rider** (modo contínuo).
- Em uma instância qualquer (ex.: no master), marque **"Usar esta instância como painel de controle"**: ela lista todos os canais com o nível de entrada (pré-trim) ao vivo, o trim atual, o **perfil do rider** e o toggle de automação de cada um.

### Ordem da lista

- A lista segue a **ordem de registro** dos plugins (geralmente a ordem das tracks na sessão logo após abrir, mas hosts não garantem isso — reordenar tracks depois, ou inserir uma instância nova, não reordena a lista).
- Use as setinhas **▲▼** ao lado de cada canal para ajustar a ordem manualmente. A ordem é **salva por canal** e sobrevive a fechar e reabrir a sessão. O modo compacto segue a mesma ordem.

## Medição de ganho

No painel, configure a **duração da medição** (padrão 5 s) e o **limite de trim ±dB** (padrão ±36, ajustável até ±60) e clique em **"Regular ganhos de todos os canais"**.

A medição é **armada por canal**: cada canal fica "aguardando sinal" e a janela só começa a contar quando o sinal chega nele — fontes que tocam em momentos isolados (tons, surdo) são capturadas quando tocarem. A medição arma no limiar de **Sensibilidade do canal** (Avançado).

Ao fim, o canal recebe `trim = target − nível medido` (limitado ao ±limite). Um canal sem sinal em 90 s fica intocado (bateria que não completa as N batidas em 90 s finaliza com o que capturou). Canal com pico abaixo de **-60 dBFS** durante a medição é marcado **"sem sinal"** e ignorado.

Há **dois modos de medição, com parâmetros independentes no painel**, mais um switch por canal para calibrar pela média ou pelo pico.

### Instrumentos (contínuos) vs bateria (percussivos)

- **Instrumentos (contínuos)** — medem por uma **janela em segundos** (padrão 5 s) que só conta enquanto há sinal acima da Sensibilidade: a média dos picos de blocos de 0.5 s.
- **Bateria (percussivos)** — medem por **contagem de batidas** (padrão 15, ajustável de 3 a 60), totalmente independente dos segundos. Um tom/surdo toca em momentos isolados, então "N batidas limpas capturadas" é o critério que faz sentido, não tempo de relógio. Usa a **média com gate dos picos das batidas** (estilo BS.1770): batidas mais de 6 dB abaixo da referência — o P90 das batidas, ou o 2º hit mais forte quando há poucas — são vazamento/ghost notes e ficam fora da média; um rimshot isolado nunca vira a referência.

### Média vs pico mais alto

Por padrão o nível medido é a **média dos picos**, não o pico máximo (mais estável, ignora um estouro isolado).

Cada canal tem, no **Avançado**, o switch **"Medir pelo pico mais alto (em vez da média)"**: ligado, o canal calibra pelo **pico máximo** capturado na janela/batidas — útil para fontes onde o momento mais alto é o que importa evitar.

### Proteção contra picos frequentes

No modo **média**, depois de calibrar pela média, o AutoTrim confere a frequência dos picos altos:

- Se picos altos forem **frequentes** — pelo menos **20% dos picos capturados** ficam mais de **3 dB acima da média** (ou seja, o ganho rodaria quente) — o nível é puxado para trás de modo que o pico mais alto caia **no máximo 3 dB acima do target**.
- Um pico avulso/esporádico (abaixo de 20%) é deixado em paz e mantém a média.

### Sensibilidade e vazamento

- A janela **só conta enquanto há sinal acima da Sensibilidade** — uma pausa não consome o tempo, então o padrão pode ser curto (5 s) mesmo para fontes com respiros.
- Canal com muito vazamento: suba a Sensibilidade dele acima do vazamento para que só o toque direto dispare a medição.

### Regulagem individual e reset

- Além do "Regular ganhos de todos os canais", há regulagem individual: botão **"Regular ganho"** no plugin do canal e botão **"Reg"** em cada linha do painel/modo compacto.
- O botão **"Zerar ganhos"** no painel coloca o Ganho de todos os canais em 0 dB e limpa rider/AGC/proteção (com **confirmação**, para um clique errado não apagar a calibração no meio da live).

## Modos automáticos ao vivo

### Rider (modo contínuo, por canal)

O **rider** mantém o nível no target com **perfis por tipo de fonte** — o preset pré-seleciona o perfil, mas você pode trocá-lo livremente no dropdown "Perfil do rider":

| Perfil | Detector | Sobe / Desce (padrão) | Ride | Sensibilidade padrão | Retorno em pausa |
|---|---|---|---|---|---|
| **Voz** | pico com hold deslizante de 2.5 s | 2.5 / 6 dB/s | ±6 dB | -45 dBFS | 2 dB/s |
| **Instrumento** | pico com hold deslizante de 3 s | 1.5 / 6 dB/s | ±4 dB | -50 dBFS | 1 dB/s |
| **Bateria** | média dos picos das últimas 8 batidas (janela 50 ms, retrigger 100 ms) | 4 / 8 dB/s | ±4 dB | -40 dBFS | 0.5 dB/s |

- A **Velocidade** do rider (taxa de subida, em dB/s) é editável em **Avançado** (0.5–15 dB/s); a descida mantém a proporção do perfil (atenuar é sempre mais rápido que subir). Os padrões seguem as referências profissionais: Voz em nível de frase (Vocal Rider "Slow" ≈ 1.5–3 dB/s), Instrumento nota a nota (Bass Rider segura o ganho dentro da nota), Bateria rápida para convergir em poucas batidas (Drum Leveler salta para o novo ganho a cada hit). Trocar o perfil (ou aplicar um preset) restaura a velocidade padrão dele.
- O ride é um **offset limitado em volta do trim medido** (não mexe no parâmetro Trim) — segurança contra o ganho fugir do calibrado.
- **Sensibilidade** (por canal): abaixo dela o rider não persegue ruído/vazamento; o offset desliza de volta ao trim medido. Trocar o perfil restaura a sensibilidade padrão dele.
- Bateria é por batida (estilo Drum Leveler): rider contínuo bombearia entre os hits.
- Zona morta de ±1 dB em volta do target: trim correto fica intocado.

### AGC (desligado por padrão)

Ativável em Avançado — funciona bem para algumas fontes, não todas. Fica **observando o tempo todo** o pico recente do canal, ignorando o que está abaixo da Sensibilidade (ruído de fundo/vazamento).

- **Quando age**: quando percebe que o nível do programa **mudou de forma permanente** — ficou mais de 3 dB fora do target pelo **Tempo do AGC** (padrão 12 s de programa, editável em Avançado de 3 a 20 s; troca de música, timbre que mudou no meio da música) — ele aplica uma correção em um passo, como uma re-medição, limitada ao **Máx. do AGC** (padrão ±10 dB, editável em Avançado de 1 a 20 dB).
- **Offset separado, em amarelo**: a correção é um **offset separado do Ganho medido**, mostrado **em amarelo** (medidor de saída, knob e visor de Ganho, badge `AGC` no canal, status no painel). O Ganho calibrado nunca é reescrito, e **3 segundos de silêncio** (abaixo da Sensibilidade) zeram a correção, devolvendo o canal ao ganho medido.
- **Bail-out de solo**: se qualquer boost do AGC estiver ativo e a saída ficar **sustentada** acima de `target + 5 dB` (solo de guitarra repentino depois de um trecho baixo), o boost cai a zero — a fonte alta prova que a calibração medida estava certa. "Sustentado" é tempo acumulado acima do limiar tolerando os vãos entre as notas: um solo palhetado dispara em ~1 s, mas um spike isolado (esbarrão no mic, pick scrape) nunca acumula o suficiente e o boost sobrevive. No bail o offset do rider também é zerado, para a queda combinada não enterrar o solo abaixo do target.
- **Avaliação pré-rider e pré-Clip Guard**: o AGC julga só a calibração base, então correções temporárias nunca mascaram a mudança permanente (o rider passa a trabalhar em volta da base corrigida). Não é um rider: nada acontece em pausas, viradas ou variações momentâneas.

### Clip Guard (ligado por padrão)

Desligável em Avançado. Se a saída passar de **0 dBFS** (clip digital real) **5 vezes em 3 segundos** (overload contínuo conta 1 evento a cada 300 ms), o plugin corta automaticamente um offset de trim para trazer o pior pico de volta ao target (corte máximo −12 dB).

- O ajuste aparece **em vermelho** (badge `CLIP` no canal, status no painel, nome em vermelho no modo compacto).
- O corte é **temporário**: após 5 s sem picos acima de 0 dBFS e com folga no sinal, ele devolve o ganho a 0.5 dB/s até zerar (sinal quente voltando reativa o corte). Uma nova medição também zera o corte.

## Painel de controle

- **Lista de canais** — nível de entrada (pré-trim) ao vivo, trim atual, perfil do rider e toggle de automação de cada canal. O medidor de entrada **não tem peak-hold**; a leitura numérica fica em uma **calha escura fixa à direita da barra**.
- **Ordem** — definida pelas setinhas **▲▼**, salva por canal (ver [Ordem da lista](#ordem-da-lista)).
- **Reset** — "Zerar ganhos" (com confirmação) zera o Ganho de todos os canais e limpa rider/AGC/proteção.
- **Modo compacto** — mostra o Ganho calibrado de cada canal como **valor somente leitura** (sem knob — é uma tela para acompanhar a live, não para ajustar), em amarelo quando o AGC está corrigindo, com o nome em vermelho quando o Clip Guard atua.

## Avançado (parâmetros por canal)

A seção **Avançado** (fechada por padrão no plugin do canal) concentra o que não é do dia a dia. As demais funções automáticas já têm interruptor próprio no card principal: **Automação** (desliga tudo — o plugin vira passthrough) e **Rider**. Medição só acontece quando você pede (botão), então não tem toggle.

| Parâmetro | Padrão | O que faz |
|---|---|---|
| **Target** | — (dBFS) | Nível de pico alvo do canal; base do trim (`trim = target − nível medido`) e referência do rider/AGC/Clip Guard. |
| **Sensibilidade** | por perfil (Voz -45 / Instrumento -50 / Bateria -40 dBFS) | Limiar abaixo do qual o sinal é ignorado — arma a medição, e o rider/AGC não perseguem ruído/vazamento. |
| **Velocidade do rider** | por perfil (Voz 2.5 / Instrumento 1.5 / Bateria 4 dB/s) | Taxa de subida do rider, 0.5–15 dB/s; a descida mantém a proporção do perfil. |
| **Tempo do AGC** | 12 s | Tempo de programa fora do target (>3 dB) que caracteriza mudança permanente antes do AGC corrigir; 3–20 s. |
| **Máx. do AGC** | ±10 dB | Correção máxima que o AGC pode aplicar; 1–20 dB. |
| **Clip Guard** | ligado | Corte automático quando a saída clipa (0 dBFS, 5×/3 s), corte máximo −12 dB. |
| **AGC** | desligado | Correção automática de mudanças permanentes de nível do programa. |
| **Medir pelo pico mais alto (em vez da média)** | desligado | Calibra pelo pico máximo capturado em vez da média dos picos. |

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

## Simulador offline (testar com lives gravadas)

O alvo `autotrim-sim` roda o **processor real** sobre um WAV/AIFF (um canal
gravado de uma live) e imprime a trajetória do ganho com timestamp: medição,
correções e bail-outs do AGC, cortes do Clip Guard, e **bumps** (variação de
ganho > 3 dB em 100 ms fora da aplicação da medição). Sai com código 0 quando
não há bumps nem clipping — dá para usar gravações reais como teste de
regressão.

```sh
scripts/docker-build.sh   # compila também o autotrim-sim
build/autotrim_sim_artefacts/Release/autotrim-sim testdata/guitarra.wav \
    --target -12 --profile instrumento --rider --agc --meas 30 --csv traj.csv
```

Coloque os arquivos em `testdata/` (ignorado pelo git). O CSV opcional tem a
trajetória por bloco (entrada, saída, trim, rider, AGC, clip) para plotar.

## Roadmap

- **Sidechain de música (estilo Vocal Rider)**: subir a voz automaticamente quando a banda cresce, alimentando o rider com um sidechain do mix. Pesquisado e especificado, deixado para depois por complexidade × retorno (ver Waves Vocal Rider: parâmetro "Music Sensitivity").
- Aproximação de "Spill" (rejeição de vazamento) mais inteligente que o threshold de sensibilidade, se necessário na prática.

## Limitações conhecidas

- O painel enxerga apenas instâncias carregadas **no mesmo processo** do host. Funciona no Logic (AUv2 in-process, padrão), Reaper, Bitwig etc.; não funciona se o host isolar plugins em processos separados (sandbox AUv3, "separate process" do Bitwig).
- Waves eMotion LV1 não aceita plugins de terceiros; para uso ao vivo, uma opção é o Waves SuperRack Performer (VST3).

## Estrutura do projeto

- `src/PluginProcessor.*` — AudioProcessor, parâmetros, `processBlock()` (metering, trim suavizado, medição, rider)
- `src/Registry.*` — registro global de instâncias + configurações do painel
- `src/Dsp.h` — matemática de ganho/rider/envelope (pura, testável sem JUCE)
- `src/Measurement.*` — orquestração da medição em massa
- `src/PluginEditor.*`, `src/LookAndFeel.h` — GUI (modo canal e modo painel)
- `tests/DspTests.cpp` — testes unitários do DSP
- `docker/`, `scripts/docker-build.sh` — build containerizado
- `archive/rust-nih-plug/` — versão anterior em Rust (arquivada)
