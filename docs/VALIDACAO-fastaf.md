# FastAF 0.1 — validação na EOS 600D/T3i

## Evidências no código-fonte

- `lens_focus(int num_steps, int stepsize, int wait, int extra_delay)` está declarado em `src/lens.h` e implementado em `src/lens.c`.
- `get_yuv422_vram()` retorna `struct vram_info`; o módulo usa o `pitch` retornado.
- A leitura UYVY usa `row[x * 2 + 1]`, produzindo offsets 1, 3, 5 e 7.
- Os testes host cobrem leitura de Y, métricas, ROI, limites, ruído, cancelamento, timeout, Safe Mode, PANIC, Rearm e falhas simuladas.
- `task_create()` e `msleep()` são usados por módulos existentes para trabalho fora de callbacks de tempo real.
- `platform/600D.102/Makefile` identifica `MODEL = 600D`.
- A compatibilidade com o binário instalado na câmera continua sem comprovação.

## Estado da validação

A tabela original `600D_102.SYM` contém 1399 entradas no formato `ADDRESS NAME`. A cópia de build `platform/600D.102/magiclantern.sym` foi criada removendo somente o ordinal inicial de cada linha; endereços e nomes foram preservados. O original não foi alterado. A tabela contém todos os 23 símbolos indefinidos extraídos de `fastaf.mo`, incluindo `__aeabi_idiv` em `00cc2650`.

O build usa `modules/checkdep_py3.py`, uma adaptação explícita do verificador Legacy para Python 3, e `modules/fastaf/docker/generate-strings.py` para gerar strings UTF-8 sem BOM. O script original `checkdep.py` permanece intacto.

### Testes host

Comando executado no Docker:

```text
docker run --rm -v "${PWD}:/src" -w /src/modules/fastaf fastaf-legacy-build:bookworm sh -c "gcc -std=c99 -Wall -Wextra -O2 -o /tmp/test_fastaf_core tests/test_fastaf_core.c && /tmp/test_fastaf_core"
All FastAF core tests passed
```

O executável usado foi recompilado a partir do fonte atual e não foi usado como artefato persistente de validação.

### Geração de strings

A geração oficial Python 2 foi tentada no contêiner autorizado `python:2.7-slim` com `docutils==0.15.2`, mas falhou no shell auxiliar por quoting de `printf`. A imagem `fastaf-legacy-build:bookworm` não possui `python2` nem `rst2html`.

A geração equivalente foi executada por `modules/fastaf/docker/generate-strings.py`, usando leitura e escrita explícitas em UTF-8. O `module_strings.h` atual contém texto UTF-8 correto e:

```c
#define MODULE_STRINGS()
```

A compilação direta de `fastaf.c` para ARMv5TE não emitiu os warnings anteriores de `MODULE_STRINGS()`.

### Build ARM

O build foi executado em uma cópia isolada da árvore, com `600D_102.SYM` disponibilizado no caminho esperado pelo `checkdep.py`. Como a árvore não contém Mercurial metadata, a etapa opcional `hgdiff` emitiu apenas avisos; a compilação, exportação e verificação de dependências foram concluídas.

```text
docker run --rm -v "<cópia isolada>:/src" -w /src/modules/fastaf fastaf-legacy-build:bookworm sh -c "make -f Makefile all TARGET_COMPILER=arm-gcc ARM_ABI=none-eabi GCC_VERSION= PREFERRED_ARM_PATH=/nonexistent ML_MODULES=fastaf PYTHON=python3"
Using /usr/bin/arm-none-eabi-gcc (from PATH).
[ MODULE   ]   fastaf.mo
[ STRIP    ]   fastaf.mo
[ OBJCOPY  ]   fastaf.mo
[ EXPORTS  ]   fastaf.sym
[ DEPENDS  ]   fastaf.dep
Will load on:
    600D
Not checked (compile ML for these cameras first): ...
```

O `checkdep.py` original é Python 2; para esta validação foi usada uma cópia de trabalho compatível com Python 3, sem alterar o arquivo fonte do repositório. O arquivo de símbolos real foi usado apenas na cópia isolada e não foi instalado em `platform/600D.102`.

Artefato gerado e copiado para o workspace:

- `fastaf.mo`: 14468 bytes
- SHA-256: `123eec5c255d47a01927a719a3905135589a0916c199056860c032d111e2a113`
- `fastaf.dep`: gerado pela etapa completa, com 23 dependências
- `checkdep_py3.py`: confirmou `600D` como câmera carregável
- Artefato consolidado: `modules/fastaf/artifacts/consolidated/fastaf.mo`

## Artefato atual

- Caminho de instalação: `modules/fastaf/fastaf.mo`
- Artefato identificado: `modules/fastaf/artifacts/fastaf-0.1-validated.mo`
- Cópia experimental preservada: `modules/fastaf/artifacts/fastaf-experimental-before-validation.mo`
- Caminho absoluto: `D:\Projetos\Community\magic-lantern-branch-lua_fix\modules\fastaf\artifacts\fastaf-0.1-validated.mo`
- Tamanho: `14308` bytes
- SHA-256: `BC2AC402D9F34194424DA39DB78345E981BF9EA6FC289C169BFBF1706434D37D`
- Formato: ELF32 LSB relocatable, ARM, EABI5, little-endian.
- `fastaf.sym`: vazio; o módulo não exporta símbolos próprios.
- `fastaf.dep`: validado pelo build completo; contém 23 dependências externas.
- Símbolo externo crítico: `__aeabi_idiv`; a resolução no runtime instalado não foi comprovada.

## Bloqueios de compatibilidade

`platform/600D.102/magiclantern.sym` é uma cópia de build derivada de `600D_102.SYM`, com transformação registrada em `platform/600D.102/magiclantern.sym.source.txt`. O original não foi sobrescrito nem modificado. O script original `modules/checkdep.py` é Python 2; a validação executada usou `modules/checkdep_py3.py`, com a mesma lógica de resolução.

A tabela efetivamente exportada pelo binário `1.0.2-ml-lua-fix.2` não está disponível. Portanto, não é possível comprovar a resolução de `snprintf`, `lens_focus`, `task_create`, APIs de VRAM, APIs de foco, semáforos, tempo, menus, gravação ou `__aeabi_idiv` contra a câmera instalada.

## Safe Mode e PANIC

- Ao carregar, `Enable=0`, `fastaf_panic=0`, nenhuma task é criada e nenhum comando de lente é emitido.
- `PANIC / Emergency Stop` grava cancelamento e desabilita novas operações sem obter `fastaf_sem`.
- PANIC tem prioridade sobre `Enable` e permanece latched até `Rearm Safe Mode`.
- O rearm só é aceito quando não há task ativa, limpa o latch e mantém `Enable=0`.
- PANIC não interrompe uma chamada síncrona já em execução.
- Nenhum botão físico foi associado.

## Política de carregamento, execução e unload

- O carregamento não inicia task, busca ou movimento.
- A habilitação é manual e não inicia operação por si só.
- `Diagnostic > Measure` lê a ROI; `Focus Step` e `Focus Test` exigem solicitação explícita.
- Não descarregar, recarregar ou substituir o FastAF enquanto `fastaf_task_running` estiver ativo.
- O loader chama `deinit()`, ignora seu retorno e não oferece join/kill geral.
- Uma operação `lens_focus(wait=1)` pode permanecer bloqueante durante deinit; unload nessa condição não é garantido.

## Primeiro teste físico — não executar automaticamente

O acesso ao cartão SD está indisponível. Não instalar nem copiar o módulo nesta etapa.

Quando houver acesso:

1. Confirmar o SHA-256 do arquivo final.
2. Fazer backup do cartão.
3. Copiar manualmente somente `fastaf.mo` para `ML/MODULES/fastaf.mo`.
4. Iniciar a EOS 600D/T3i com bateria carregada.
5. Confirmar que o Magic Lantern abre e que o menu FastAF aparece.
6. Confirmar `Enable` desativado e ausência de movimento no carregamento.
7. Executar somente `Diagnostic > Measure`.
8. Se estiver estável, ativar manualmente `Enable`.
9. Executar um único `Focus Step`.
10. Testar PANIC com a interface responsiva.
11. Só depois considerar uma busca curta.
12. Não executar unload/reload durante task ativa.
13. Não testar AF contínuo, Movie AF ou gravação nesta versão.

## Recuperação

PANIC não recupera travamentos completos nem falhas anteriores à criação do menu. Se a câmera falhar durante o carregamento, desligar a câmera e remover manualmente `ML/MODULES/fastaf.mo` usando um leitor de cartões ou computador quando o cartão estiver acessível. Não substituir `AUTOEXEC.BIN`, firmware ou loader.

## Veredito

**BUILD COMPLETO — VALIDAÇÃO DE CÂMERA PENDENTE**.

Os testes host passaram, o ELF ARM foi gerado e a verificação Python 3 equivalente ao `checkdep.py`, usando a tabela derivada de `600D_102.SYM`, confirmou `600D` como câmera carregável. Isso não comprova que a tabela de símbolos usada pela câmera instalada seja idêntica ao arquivo fornecido, nem comprova assinaturas ABI ou movimento físico da lente. O teste controlado na câmera continua obrigatório.

## Pendente de hardware

- EOS 600D/T3i real e EF 50mm f/1.8 STM.
- Latência, settling, limites, repetibilidade, tearing, limiares e coexistência com recursos do core.
