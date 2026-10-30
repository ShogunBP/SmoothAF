# FastAF 0.1 — revisão de código

**Data:** 2026-10-29  
**Árvore:** Legacy Lua Fix local, sem metadados Git  
**Alvo:** EOS 600D/T3i, firmware 1.0.2, DIGIC IV, EF 50mm f/1.8 STM

## A. Problemas encontrados

- **Alta — Makefile incompatível com a interface local.** O módulo usava `modules/Makefile`, `BUILD_DIR` e um alvo `module_complete` não localizado nesta árvore. O padrão confirmado em `modules/bolt_rec/Makefile` usa `MODULE_OBJS=fastaf.o` e `modules/Makefile.modules`.
- **Alta — risco de duas tarefas.** A reserva de execução precisava ocorrer antes de `task_create()`. O código agora usa semáforo e reserva antecipada.
- **Alta — descarregamento com tarefa pendente.** O loader Legacy documenta que unload não é limpo e pode deixar tarefas apontando para código liberado. A leitura direta de `src/module.c` confirma que o loader chama `deinit()`, ignora o valor retornado e marca o módulo inválido; não há join/kill geral de task. O deinit desabilita, cancela, espera cooperativamente por até 3 s e remove o menu, mas isso não torna o unload seguro se a task continuar ativa.
- **Alta — cancelamento após medição.** O controlador podia avaliar um score depois de `measure()` e retornar `LOCKED` sem uma nova consulta a `abort()`. Agora há uma verificação entre a medição e `fastaf_hc_on_score()`.
- **Média — conflitos de foco incompletos.** O bloqueio agora consulta Follow Focus, Rack Focus e Focus Stacking, além de MF, Continuous AF, LiveView e gravação.
- **Média — documentação contraditória da indexação UYVY.** A expressão efetiva corrigida é `row[x * 2 + 1]`; o teste sintético confirma Y nos offsets 1, 3, 5 e 7.

## B. Correções realizadas

- `modules/fastaf/Makefile`: alinhado ao padrão `Makefile.modules`; nenhum build ARM foi executado.
- `modules/fastaf/fastaf_core.h`: indexação UYVY simplificada e comentário corrigido; ROI mantém alinhamento par, pitch retornado em runtime e limites conservadores.
- `modules/fastaf/fastaf.c`: semáforo de controle, reserva antes de criar tarefa, tratamento do retorno de `task_create()`, cancelamento entre comandos, limites de busca, `wait=1`, bloqueios de gravação e conflitos de foco, estado inicial desativado e deinit cooperativo.
- `modules/fastaf/tests/test_fastaf_core.c`: regressão sintética de UYVY, cancelamento/timeout durante medição e cancelamento após movimento, sem confundir a fórmula antiga com o resultado da implementação atual.
- `README.rst` e `VALIDACAO.md`: limitações, concorrência e validação local atualizadas.

## C. Problemas descartados

A conclusão anterior de que `fastaf_y()` correta deveria conter deslocamento extra para pixels ímpares foi descartada. Em UYVY, os offsets absolutos de Y são 1, 3, 5 e 7; `row[x * 2 + 1]` é a fórmula correta.

Não foi implementado AF contínuo, Movie AF, RAW AF, detecção de rostos ou disparo automático.

## D. Testes

O executável host já presente foi executado antes desta revisão, mas não foi recompilado a partir do fonte atualizado. Portanto, aquele resultado não valida os novos testes de cancelamento.

A verificação de compiladores host nesta sessão não encontrou `gcc` nem `cl`. Não houve instalação de dependências. O código-fonte mantém comandos reproduzíveis para GCC/Clang e MSVC; a recompilação atualizada permanece pendente.

O teste antigo produziu:

```text
modules/fastaf/tests/test_fastaf_core.exe
All FastAF core tests passed
```

Não foi executado `make`, não foi gerado `.mo` e não houve teste na câmera.

## E. Segurança

- No máximo 16 comandos reais e 4 inversões por busca.
- `lens_focus()` síncrono, fora de VSYNC, um comando por vez.
- Stop impede comandos futuros; uma chamada síncrona já iniciada pode terminar.
- Timeout global é verificado entre chamadas; não interrompe `lens_focus()` em voo.
- Busca cancelada não retorna `LOCKED`, inclusive quando o cancelamento ocorre durante `measure()`.
- Tarefa única reservada antes da criação.
- Enable inicia desativado e o carregamento não movimenta a lente.
- O core não oferece arbitragem global entre todos os módulos; o FastAF apenas aplica os bloqueios públicos conhecidos.

## F. Preparação para compilação

O Makefile está estaticamente alinhado ao padrão local de módulos. A compatibilidade efetiva com o toolchain ARM, símbolos exportados, `MODULE_MAJOR/MINOR` e port `600D.102` ainda precisa de um build posterior autorizado. Esta revisão não declara que o Makefile foi compilado.

## G. Limitação de unload e política operacional

A leitura de `src/module.c` confirma que o loader chama `deinit()`, ignora o
valor de retorno e não fornece join/kill geral para tasks de módulos. O FastAF
desabilita novas operações, solicita cancelamento e espera cooperativamente por
até 3 segundos. `lens_focus(wait=1)` pode permanecer bloqueante durante parte
desse intervalo. Se a task continuar ativa, a espera não impede necessariamente
o unload; descarregar, recarregar ou substituir o módulo durante uma operação
pode causar travamento ou comportamento indefinido. Não se afirma que brick
permanente seja impossível.

**Política:** não descarregar, recarregar ou substituir o FastAF enquanto houver
qualquer operação ou `fastaf_task_running` ativo. O desligamento da câmera não é
tratado como equivalente a unload dinâmico.

## H. Pendências de hardware

Ainda precisam ser medidos na EOS 600D com EF 50mm STM: latência de `lens_focus()`, settling, backlash, repetibilidade dos passos 1–3, comportamento nos limites, tearing do buffer, limiares de confiança, baixo contraste, exposição variável e coexistência com recursos do core.

## I. Checklist de aceite

| Item | Estado |
|---|---|
| Indexação UYVY e métricas host | PASSOU |
| ROI, pitch e limites estáticos | PASSOU |
| Cancelamento, timeout e falhas simuladas | PASSOU no Docker |
| Limites da busca e direção não nula | PASSOU no Docker |
| Makefile revisado e executado | PASSOU no Docker |
| Build ARM / `.mo` | GERADO; inspeção pendente contra símbolos da câmera |
| Compatibilidade com Legacy Lua Fix/binário instalado | PENDENTE |
| Teste físico na câmera e lente | PENDENTE |

## Veredito

**BUILD GENERATED — VALIDATION PENDING** — os testes host foram recompilados
no Docker e passaram após a inclusão de Safe Mode/PANIC; o build ARM gerou e
permitiu inspecionar `fastaf.mo`. Ainda faltam validação contra o binário Legacy
Lua Fix instalado, inspeção do arquivo de símbolos da câmera e teste físico. O
unload dinâmico continua sem garantia durante task ativa.
