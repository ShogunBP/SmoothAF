# SmoothAF V2 — B2-C1a: auditoria canônica host-only de SMTH003

**Status:** concluída para revisão e encerramento; host-only; sem alteração de `src/`, algoritmo, gates, budgets, STOP/PANIC, recovery, B2-B ou release. Nenhum commit/push foi feito antes desta autorização.

## 1. Artefatos e reprodução

A fonte local `logs/SMTH003.LOG` contém 367 linhas TSV e SHA-256 `E08D0D90E6BA6791CDC36A5C5CFBE614FCDAD1AF9614B240151132D4448B36E7`. O extrator canônico é `tools/b2c1a_canonical_audit.py`; os testes estão em `tests/test_b2c1a_canonical_audit.py`. `logs/` e `artifacts/` são caminhos ignorados pela política do repositório; o CSV/JSON permanecem gerados localmente e reproduzíveis, mas não entram neste commit.

```powershell
python -B tools/b2c1a_canonical_audit.py `
  --log logs/SMTH003.LOG `
  --csv artifacts/SMTH003.canonical.csv `
  --json artifacts/B2-C1a.json
```

O CSV preserva `line`, `timestamp`, `event` e a ordem original, inclusive timestamps iguais. Não cria frame ID: o log não possui esse identificador. A posição é somente posição lógica acumulada por `direction * step`; não é posição óptica nem encoder (`src/smoothaf_episode.h:50-59`). O quarto slot do payload de `MEASUREMENT` é Laplacian, não frame ID (`src/smoothaf_logger.h:497-498`; `src/smoothaf_logger.c:278-287`).

O timestamp de medição é obtido após `smoothaf_measure()` retornar (`src/smoothaf.c:2002-2007`), portanto é timestamp de conclusão da medição, não captura atômica de frame ou exposição.

## 2. Semântica de produção auditada

- Payload `MEASUREMENT`: score, mean/luminância, SAD, Laplacian, valid, confident (`src/smoothaf_logger.h:497-498`; `src/smoothaf_logger.c:278-287`).
- Logger publica a amostra armada com os seis campos (`src/smoothaf.c:2019-2026`).
- A métrica calcula SAD, Laplacian e mean na mesma ROI; o score seleciona SAD ou Laplacian conforme a métrica, sem normalização por luminância/exposição (`src/smoothaf_core.h:377-435`; `src/smoothaf.c:1459-1495`).
- `best` é máximo individual estrito: `score > best`, com posição atual (`src/smoothaf_episode.h:182-196`). O caminho Current Vlog também atualiza o máximo em `score > af->best` (`src/smoothaf_monitor.h:1497-1512`). Não é mediana nem agregador robusto.
- AUTO é evidência direcional transitória e restaura `best`, `previous_score`, `post_move_score`, `score_delta` e posição para a origem (`src/smoothaf_episode.h:61-80`). O deadband é `max(score / 64, 24)` (`src/smoothaf_core.h:83-87`).
- A confirmação de controle exige duas confirmações no caminho Legacy HC (`src/smoothaf_core.h:466-479`); a monitorização ambiental usa limiar de mean `32`, amostras sustentadas e estados de referência (`src/smoothaf_monitor.h:1768-1776`, `1832-1978`). Amostras pós-movimento são verificadas para iluminação antes da incorporação ambiental (`src/smoothaf.c:2061-2074`, `2098-2125`).

## 3. Reconstrução canônica

A reconstrução produz os oito episódios AF 2–9, ignorando corretamente as séries D1 iniciais que reutilizam IDs 4 e 5.

| Episódio | Comandos | Posições lógicas | Eventos ambientais | Terminal | LIMIT |
|---:|---:|---|---:|---:|---|
| 2 | 7 | `-3,0,3,0,-3,-6,-3` | — | 11 `SEARCH_LIMIT` | — |
| 3 | 2 | `3,0` | linha 140 | 3 `UNCERTAIN` | — |
| 4 | 1 | `-3` | linha 149 | 3 `UNCERTAIN` | — |
| 5 | 8 | `3,0,-3,0,3,6,9,6` | — | 4 `CONVERGED` | — |
| 6 | 1 | `-3` | linha 227 | 3 `UNCERTAIN` | — |
| 7 | 8 | `3,0,-3,0,3,0,-3,0` | — | 11 `SEARCH_LIMIT` | — |
| 8 | 8 | `-3,0,3,0,-3,0,3,0` | — | 6 `BLOCKED` | `[2,8,0,0,0,0]` (`SESSION_CMDS`) |
| 9 | 1 | `3` | linha 365 | 3 `UNCERTAIN` | — |

`SEARCH_LIMIT` de EP7 é um resultado terminal sem evento `LIMIT`; EP8 possui explicitamente `LIMIT` tipo 2. Os eventos ambientais são preservados. Não se infere que um resultado terminal posterior torne prefixos anteriores inválidos.

## 4. Auditoria da margem 357

A fórmula histórica, reproduzida sem seleção retrospectiva nova, é:

```text
within_visit_mad_max = 119
repeated_position_delta_max = 187
direction_arrival_delta_max = 187
three_times_mad = 3 * 119 = 357
peak_margin = max(357, 187, 187) = 357
```

Esses valores estão confirmados como reprodução da fórmula do harness B2-B, mas **357 continua não calibrada** como limiar geral. A classificação desta auditoria é explicitamente `INSUFFICIENT_EVIDENCE`: uma sessão, sem frame ID, sem snapshot de exposição associado e com variação intervisita maior que 357 nos episódios de validação.

### EP2, posição −3

A visita não-AUTO do comando 4 contém, nas linhas 106, 107 e 109, scores `21116, 22187, 21198`:

- amplitude com todas as amostras válidas: `1071`;
- MAD com todas: `82`;
- tail-3 do harness: `21116, 22187, 21198`, mediana `21198`, MAD `82`, amplitude `1071`;
- excluir a primeira e depois tail-3 (política C/D, que só exclui com pelo menos quatro amostras) não muda essa visita, pois ela tem exatamente três amostras;
- a visita posterior ao retorno, comando 6, tem apenas `20214, 20409`: amplitude `195`, não satisfaz o mínimo de três timestamps e não é elegível para comparação completa.

O `22187` pode elevar o máximo individual registrado e o `best` de produção, enquanto a agregação temporal do harness não o transforma em referência robusta. O final `20409` não substitui o best histórico: `EPISODE_END` registra best `22187@-3`, final `20409@-3`. Isso é consistente com máximo individual e não prova, por si só, falsa perda de foco.

### Comparações solicitadas

As estatísticas abaixo usam a **mediana dos três últimos scores válidos/confidentes de cada visita** (`harness_tail3`); a diferença absoluta é `max(medianas) - min(medianas)` e a relativa é a diferença absoluta dividida pela média das medianas. Visitas com menos de três timestamps distintos são `INSUFFICIENT_EVIDENCE`.

| Episódio/posição | Medianas tail-3 das visitas não-AUTO | Diferença absoluta | Relativa |
|---|---:|---:|---:|
| EP2 −3 | `21198`; segunda incompleta `20311,5` | `INSUFFICIENT_EVIDENCE` para comparação completa | — |
| EP2 0 | nenhuma visita não-AUTO | `INSUFFICIENT_EVIDENCE` | — |
| EP5 +3 | uma visita | `INSUFFICIENT_EVIDENCE` | — |
| EP5 +6 | `46142`; `46329` | `187` | `0,404%` |
| EP7 0 | `11870`; `11295` | **`575`** | `4,964%` |
| EP7 −3 | uma visita | `INSUFFICIENT_EVIDENCE` | — |
| EP8 0 | `11360`; `11880` | **`520`** | `4,475%` |

Assim, os valores preservados são **575 no EP7** e **520 no EP8**, ambos definidos como amplitude entre medianas tail-3 de visitas não-AUTO na mesma posição lógica. Para EP8, isso não deve ser chamado de histerese: não há frame ID, exposição atômica ou isolamento físico da causa.

## 5. Duplicatas e independência

O CSV marca somente repetição da tupla métrica `(score, mean, SAD, Laplacian, valid, confident)` e não a descarta nem a promove a frames independentes. Repetições exatas por episódio: EP2 2, EP3 0, EP4 0, EP5 1, EP6 0, EP7 3, EP8 2, EP9 0. Há pares consecutivos com mesmo score e outros campos diferentes em EP5 e EP8.

A conclusão é:

- duplicação exata de registro: possível no nível dos campos registrados;
- reutilização do mesmo frame: possível, mas não comprovada;
- coincidência de métricas: possível, mas não comprovada;
- evidência independente de frame: ausente.

Timestamps diferentes não resolvem a identidade: são conclusões de medição. As comparações temporais são proxies, não prova de independência óptica.

## 6. EP8 — replay causal em streaming

O avaliador consome linhas em ordem. A primeira oportunidade causal aparece na linha 321, timestamp 358085, após o comando índice 3, na posição lógica 0. A decisão usa somente observações já disponíveis:

- −3: tail-3 `8056,8103,8059`, mediana `8059`;
- +3: `10608,10037,10081`, mediana `10081`;
- 0, visita índice 1: tail-3 `11350,11318,11408`, mediana `11350`;
- 0, visita índice 3: `11981,11981,11846`, mediana `11981`;
- centro agregado `11665,5`; margem `1584,5`.

Resultado da hipótese A:

- classificação: `REPLAY_FIEL_COM_PARADA_ANTECIPADA`;
- primeiro comando não executado na simulação: índice 4, linha 326, timestamp 358506;
- comandos posteriores evitados na trajetória real: índices 4, 5, 6 e 7;
- tempo do prefixo decisório até o fim da trajetória real: 3094 ms;
- observação futura necessária: não.

A oportunidade de parada é fiel ao prefixo observado, mas o resultado operacional não foi testado: a trajetória real continuou. Logo, não é `CONVERGED` de produção nem validação de HOLD.

A hipótese B, aguardando visitas adicionais, é classificada `CONTRAFACTUAL`: os dados adicionais existem porque a trajetória real continuou, e não existe evento de confirmação/HOLD de produção nesse prefixo. A auditoria não converte essa hipótese em resultado observado.

## 7. Score × luminância e métrica

Para as 129 medições AF, a correlação temporal agregada é aproximadamente `-0,26425`; por episódio: EP2 `-0,64141`, EP3 `-0,73681`, EP4 `-0,99223`, EP5 `-0,84517`, EP6 `+0,99645`, EP7 `+0,25649`, EP8 `-0,70589`, EP9 `+0,33660`. As duas séries D1 têm 30 amostras cada: série 4 `-0,89786`, série 5 `-0,51598`. A correlação não estabelece causalidade.

O produto score×luminância, intervalos e contagens estão em `artifacts/B2-C1a.json`. A implementação real calcula SAD, Laplacian e mean separadamente na mesma ROI; não há divisão, normalização ou compensação explícita por luminância/exposição (`src/smoothaf_core.h:382-435`). Portanto, não é válido interpretar score como normalizado por luminância.

## 8. Premissas B2-B

| Premissa | Status B2-C1a |
|---|---|
| A fórmula histórica reproduz 357 | **CONFIRMADA como fórmula computacional** |
| 357 é margem geral/calibrada | **REFUTADA / INSUFFICIENT_EVIDENCE**; permanece não calibrada |
| EP2 possui duas visitas completas em −3 | **REFUTADA**; a segunda tem duas amostras |
| best é agregador robusto | **REFUTADA**; é máximo individual estrito |
| EP8 tem oportunidade causal após comando 3 | **CONFIRMADA no prefixo; parada operacional contrafactual** |
| EP8 teve resultado operacional HOLD testado | **REFUTADA**; a trajetória real não parou |
| oito pares provam frames independentes | **REFUTADA**; contagem exata é menor e frame ID ausente |
| EP7 SEARCH_LIMIT exige evento LIMIT | **REFUTADA**; resultado e LIMIT são eventos distintos |
| EP8 BLOCKED é SESSION_CMDS | **CONFIRMADA**; LIMIT tipo 2, 8 comandos |
| eventos ambientais autorizam causalidade física | **INCONCLUSIVA**; são classificações de gate |
| score/luminância observados demonstram causalidade | **REFUTADA** como inferência; correlação é descritiva |

## 9. Testes e decisão de encerramento

Testes específicos e suíte host devem ser executados antes do commit:

```text
python -B tests/test_b2c1a_canonical_audit.py
python -B tests/test_b2b_host_analysis.py
.\build-smoothaf.ps1 -TestOnly
```

Os testes específicos cobrem ordem original e timestamps iguais, payload Laplacian, ausência de frame ID, posições, eventos ambientais, reconstrução determinística, fórmula 357, políticas EP2, replay causal por prefixo e score×luminância.

**Recomendação para B2-C1a:** encerramento host-only aprovado. **NO-GO para B2-C1b operacional/HOLD**. A base canônica e o replay estão prontos para revisão, mas a evidência atual não sustenta margem operacional geral nem critério HOLD validado. Não calibrar retrospectivamente 357 e não tratar `HOLD_CANDIDATE` como `CONVERGED` de produção.
