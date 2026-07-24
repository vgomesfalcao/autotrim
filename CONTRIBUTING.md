# Contribuindo com o AutoTrim

Obrigado pelo interesse em contribuir! Este documento explica como reportar
problemas, propor mudanças e enviar código.

Ao participar, você concorda em seguir o [Código de Conduta](CODE_OF_CONDUCT.md).

## Índice

- [Reportar bugs](#reportar-bugs)
- [Propor funcionalidades](#propor-funcionalidades)
- [Ambiente de desenvolvimento](#ambiente-de-desenvolvimento)
- [Rodando os testes](#rodando-os-testes)
- [Testando com áudio real (simulador)](#testando-com-áudio-real-simulador)
- [Padrões de código](#padrões-de-código)
- [Enviando um Pull Request](#enviando-um-pull-request)
- [Licença das contribuições](#licença-das-contribuições)

## Reportar bugs

Abra uma [issue](https://github.com/vgomesfalcao/autotrim/issues) usando o
template de bug. Inclua:

- O que aconteceu e o que você esperava.
- Passos para reproduzir (host/DAW, sistema, formato AU ou VST3).
- Se for de comportamento de áudio (bump de ganho, medição, rider, AGC), o
  ideal é anexar ou descrever o material — o [simulador offline](#testando-com-áudio-real-simulador)
  transforma uma gravação em um caso reproduzível.

## Propor funcionalidades

Abra uma issue com o template de funcionalidade descrevendo o problema que
você quer resolver (não só a solução). Para mudanças grandes, discuta na issue
antes de abrir um PR — evita retrabalho.

## Ambiente de desenvolvimento

O build principal é **containerizado** — você só precisa de Docker para
compilar e rodar os testes (VST3 no Linux). O **Audio Unit (AU)** só compila
no macOS.

```sh
# Linux (Docker): configura, compila (VST3 + simulador) e ...
scripts/docker-build.sh          # ... deixa o VST3 em build/AutoTrim_artefacts/
scripts/docker-build.sh test     # ... compila e roda os testes de DSP
```

No **macOS** (Xcode Command Line Tools + CMake), para AU + VST3 universais:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release
```

Detalhes de instalação e validação (`auval`, Logic) estão no
[README](README.md#build-no-macos-au--vst3-para-o-logic).

## Rodando os testes

```sh
scripts/docker-build.sh test
```

Deve terminar com `todos os testes passaram`. **Todo PR precisa passar nos
testes.** A CI não roda os testes automaticamente ainda; rode localmente antes
de enviar.

## Testando com áudio real (simulador)

O alvo `autotrim-sim` roda o *processor real* sobre um WAV/AIFF (um canal
gravado de uma live) e reporta a trajetória do ganho com timestamp, marcando
**bumps** (subida de ganho inexplicada) e clipping. Use gravações reais como
teste de regressão:

```sh
scripts/docker-build.sh   # compila também o autotrim-sim
build/autotrim_sim_artefacts/autotrim-sim testdata/canal.wav \
    --target -12 --profile instrumento --agc --meas 5
```

Coloque os arquivos em `testdata/` (ignorado pelo git).

## Padrões de código

- **Toda lógica automática nova nasce como um `struct` puro e testável em
  `src/Dsp.h`, com testes em `tests/DspTests.cpp`, antes de entrar no
  `processBlock`.** As máquinas de estado do áudio (medição, rider, AGC, Clip
  Guard, detector de hits) são todas puras justamente porque é onde os bugs
  aparecem — o `processBlock` só orquestra: liga entradas e aplica os
  resultados nos atômicos compartilhados.
- **Thread de áudio não aloca, não bloqueia, não pega locks.** O registro
  global (`src/Registry.*`) só é travado nas threads de mensagem/estado; a
  thread de áudio toca apenas nos atômicos de `ChannelShared`.
- **Comentários de código em inglês; textos de interface em português.**
  Explique *por quê*, não *o quê*.
- Strings de UI com acentos passam pelo helper `utf8()` (o `String(const char*)`
  do JUCE trata literais como Latin-1).
- Siga o estilo do arquivo em que você está mexendo (indentação, nomes, idioma
  dos comentários). O projeto usa C++17.
- Mantenha os `README.md` atualizados quando mudar comportamento visível.

## Enviando um Pull Request

1. Faça um fork e crie um branch a partir de `main`.
2. Faça a mudança com testes (veja os padrões acima).
3. Rode `scripts/docker-build.sh test` — precisa passar.
4. Escreva mensagens de commit claras (o quê e por quê). PRs pequenos e
   focados são revisados mais rápido.
5. Abra o PR descrevendo o problema resolvido e como testou. Se for de áudio,
   diga como validou (simulador, teste em DAW).

## Licença das contribuições

O AutoTrim usa o [JUCE](https://juce.com) sob a licença gratuita, que exige
que o projeto seja **GPL v3** (veja [LICENSE](LICENSE)). Ao enviar uma
contribuição, você concorda em licenciá-la sob os mesmos termos (GPL v3).
