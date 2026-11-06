# SmoothAF V2 — B2-B: convergência, granularidade STM e FINE/HOLD

**Status:** análise host-only exploratória; nenhuma alteração de comportamento de produção autorizada.

**Evidência principal:** `logs/SMTH003.LOG`, SHA-256 `E08D0D90E6BA6791CDC36A5C5CFBE614FCDAD1AF9614B240151132D4448B36E7`.

**Ferramenta:** `tools/b2b_host_analysis.py`. Todos os resultados de estratégias e adversários abaixo são `SIMULATION / HOST-ONLY`; não são validação de câmera.

## 1. Decisão executiva

- **GO para encerrar B2-B como análise host-only/documental.** A reconstrução, a calibração congelada, a auditoria de fontes e a distinção metodológica do EP8 estão registradas abaixo.
- **NO-GO para integração operacional, build ARM, instalação, D2, ACTIVE CONTINUOUS físico ou ajuste da margem.** A evidência ainda não comprova deslocamento óptico, identidade de frame, exposição efetiva por amostra, histerese ou generalização para outras cenas.
- A margem **357** permanece experimental e `NOT_CALIBRATED_ON_T3I`. Ela foi derivada somente de EP2 e EP5; EP7 e EP8 são validação congelada, não recalibração.
- A linguagem correta é **“candidato sustentado por 2 casos nesta sessão”** (EP5 e EP7). Não há “HOLD validado” nem `CONVERGED` operacional novo.
- EP8 é um **replay fiel com parada antecipada até o primeiro comando divergente**: a decisão usa observações já registradas até o comando índice 3. A afirmação de que a câmera teria deixado de executar o comando índice 4 e de quais seriam as consequências posteriores é contrafactual.

A API Legacy usada pelo SmoothAF é `lens_focus(int num_steps, int stepsize, int wait, int extra_delay)` e a chamada de produção permanece `lens_focus(direction, step, 1, 0)` (`docs/references/magic-lantern-branch-lua_fix/src/lens.h:249-256`; `src/smoothaf.c:1601`). O `step` é limitado pela implementação Legacy a `-3..3` (`docs/references/magic-lantern-branch-lua_fix/src/lens.c:649-680`); isso descreve o comando Canon, não deslocamento óptico calibrado.

## 2. Reconstrução de SMTH003

| Episódio | Duração | Comandos | Trajetória lógica inferida | Melhor / final registrado | Resultado terminal |
|---:|---:|---:|---|---|---|
| 2 | 4985 ms | 7 | `0→-3→0→3→0→-3→-6→-3` | `22187@-3` / `20409@-3` | `SEARCH_LIMIT` |
| 3 | 1397 ms | 2 | `0→3→0` | `20081@0` / sentinel `-1` | `UNCERTAIN` + `ENVIRONMENTAL` |
| 4 | 669 ms | 1 | `0→-3` | `20180@0` / sentinel `-1` | `UNCERTAIN` + `ENVIRONMENTAL` |
| 5 | 6070 ms | 8 | `0→3→0→-3→0→3→6→9→6` | `46118@6` / `46476@6` | `CONVERGED` registrado |
| 6 | 621 ms | 1 | `0→-3` | `6991@0` / sentinel `-1` | `UNCERTAIN` + `ENVIRONMENTAL` |
| 7 | 5752 ms | 8 | `0→3→0→-3→0→3→0→-3→0` | `11947@0` / `11379@0` | `SEARCH_LIMIT` |
| 8 | 5889 ms | 8 | `0→-3→0→3→0→-3→0→3→0` | `11833@0` / `11833@0` | `BLOCKED` + `LIMIT SESSION_CMDS` |
| 9 | 667 ms | 1 | `0→3` | `7356@0` / sentinel `-1` | `UNCERTAIN` + `ENVIRONMENTAL` |

Todos os comandos registrados têm origem API com resultado `1`, mas isso indica aceitação/conclusão do caminho API; não comprova deslocamento físico ou melhora óptica (`docs/RESEARCH.md:36-60`; `README.md:86-99`). EP8 não tem `TIMEOUT`: o evento terminal é `BLOCKED` e o `LIMIT` é `SESSION_CMDS` (`type=2`). Há uma colisão de identidade entre D1 série 5 e episódio 5; o harness separa eventos por tipo e trajetória, não apenas pelo identificador.

## 3. F1–F10

As etiquetas F1–F10 não são símbolos formais do repositório; a tabela mantém as definições do pedido original e explicita o que o log permite ou não concluir.

| Hipótese | Classificação | Evidência | Limitações e incertezas |
|---|---|---|---|
| **F1 — EP2/5/7/8 têm busca; 3/4/6/9 são ambientais** | **CONFIRMADA no nível do log** | Os quatro episódios têm comandos; 3/4/6/9 têm resultado 3 e evento `ENVIRONMENTAL`. | “Ambiental” é a classificação do controlador/log, não prova de que a causa física foi exposição, iluminação, enquadramento ou foco. |
| **F2 — os quatro terminam na melhor posição lógica observada** | **CONFIRMADA, com ressalva** | A posição final lógica coincide com a posição melhor registrada nos quatro episódios. | A posição é soma `direção × step`, não posição física. EP5 tem amostra bruta `46476`, enquanto o resumo persistido é `46118`; máximo bruto e valor persistido não são sinônimos. |
| **F3 — EP2/7 são SEARCH_LIMIT, EP5 CONVERGED e EP8 BLOCKED/LIMIT** | **CONFIRMADA no nível dos eventos** | Enums e payloads correspondem; EP8 tem `LIMIT type=2`, não `TIMEOUT`. | `CONVERGED` de EP5 é o resultado registrado do controlador; não é uma validação independente da óptica. |
| **F4 — D1 tem transiente inicial e cauda estável; causa do transiente** | **INCONCLUSIVA** | D1-4: primeiro score `35827`, mediana da cauda `+7,89%`, cauda `σ≈0,146%`, mean `101→93`. D1-5: primeiro score `6520`, cauda `+0,64%`, `σ≈0,311%`, mean `119→115`. | O log não separa exposição, atualização do LiveView, enquadramento, conteúdo do ROI ou foco manual. D1 estacionário não equivale a settling depois de movimento. |
| **F5 — diferença EP8 por direção de chegada de aproximadamente 4,7%** | **INCONCLUSIVA** | A agregação por visita/direção dá `575,5` pontos e `4,94%` entre medianas de chegadas opostas em posição lógica 0. | O valor é próximo, mas não confirma `~550/4,7%`; ordem temporal, exposição, transiente e amostragem não estão isolados. Não se pode atribuir o efeito à histerese mecânica. |
| **F6 — o pico tem largura próxima de um comando step 3** | **INCONCLUSIVA fisicamente; padrão lógico observado** | Quedas do centro contra o maior vizinho: EP2 `~40%`, EP5 `~47%`, EP7 `~15,5%`, EP8 `~13,6%`. | Isso mostra contraste entre posições lógicas espaçadas por 3. Não mede largura óptica, nem prova que steps 1/2 sejam inúteis. |
| **F7 — AUTO pode descartar candidato superior** | **CONFIRMADA como comportamento de desenho** | `finish_auto_probe()` restaura `best`, `previous_score` e `post_move_score` ao score inicial e põe `best_position=current_position` (`src/smoothaf_episode.h:61-72`). | O comentário define AUTO como evidência direcional, não como evidência do algoritmo. É uma política de desenho observada, não defeito confirmado. |
| **F8 — gate ambiental pode usar referência envelhecida/confundir foco** | **INCONCLUSIVA** | O gate compara referência global com limiar `32` e saída `24` (`src/smoothaf_monitor.h:1768-1775`; constantes `:27-32`). O fluxo classifica a amostra pós-movimento antes de incorporá-la à referência (`src/smoothaf.c:2061-2073`, `:2098-2125`). | Há mudanças de mean entre posições, mas faltam metadados de exposição, frame e enquadramento para estabelecer causalidade. |
| **F9 — há medições consecutivas idênticas** | **CONFIRMADA como igualdade métrica, não como mesmo frame** | Há 10 pares de `score/mean/valid/confident` idênticos em EP2/5/7/8, com timestamps distintos. | O logger não tem frame ID. Releitura do mesmo buffer e coincidência métrica permanecem indistinguíveis. |
| **F10 — a primeira medição pós-movimento frequentemente difere** | **INCONCLUSIVA para uma regra universal** | Em 30 visitas, 11 diferem mais de 2% entre a primeira amostra e a mediana posterior; EP5 mostra transientes de `+22,5%`, `−10,1%` e `+7,8%`. | O achado justifica descarte condicional a ser testado; não autoriza descarte incondicional nem prova que a diferença seja settling. |

A hipótese anteriormente aventada de que AUTO avança sem limpar `awaiting_result` não se confirma na auditoria completa: o fluxo chama `smoothaf_episode_prepare_score()` antes de processar a amostra AUTO (`src/smoothaf.c:2159`; `src/smoothaf_episode.h:182-196`).

## 4. Margem experimental 357

### 4.1 Origem e interpretação

A calibração congelada usa somente EP2 e EP5:

- MAD intravisita máximo: **119**;
- maior diferença observada entre visitas/direções elegíveis: **187**;
- margem combinada usada pelo harness: `max(3×119, 187, 187) = 357`;
- `MIN_TEMPORAL_SAMPLES = 3`; timestamps distintos são proxy temporal, não identidade de frame.

A margem é descritiva para esta sessão SMTH003. As porcentagens abaixo não são nova calibração:

| Episódio | Score de referência | MAD intravisita máx. | Diferença máx. entre visitas repetidas* | Diferença de chegada por direção** | Percentual da diferença de direção | 357 como % do score | Diferenças que superam 357 | Quantidade/qualidade |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 2 | 22187 | 82 | — | 282 | 2,23% | 1,61% | Nenhuma das medidas completas; 282 < 357 | 7 visitas/comandos, 23 amostras; uma visita não tem 3 amostras válidas/confidentes. Comparação direcional tem apenas um par em posição 0 e inclui AUTO; qualidade baixa. |
| 5 | 46118 | 119 | 187 | 187 | 0,40% | 0,77% | Não | 8 visitas, 32 amostras; quatro visitas não-AUTO completas, com retorno em posição 6. Melhor suporte da calibração. |
| 7 | 11947 | 84 | 575 | 190 | 1,67% | 2,99% | 575 entre visitas não-AUTO; 190 na agregação direcional de todas as chegadas | 8 visitas, 30 amostras; quatro visitas não-AUTO completas. Há forte variação entre retornos, mas agregação por direção fica diluída por AUTO. |
| 8 | 11833 | 47 | 520 | **575,5** | **4,94%** | 3,02% | 520 e 575,5 | 8 visitas, 29 amostras; quatro visitas não-AUTO, todas com pelo menos 3 amostras. Há duas chegadas em cada direção para a posição 0. |

\* “Visitas repetidas” usa as visitas não-AUTO com suporte temporal suficiente e compara a maior diferença entre medianas de visitas na mesma posição lógica. EP2 não fornece duas visitas completas comparáveis.

\*\* “Chegada por direção” agrega as medianas por direção na mesma posição lógica. Para EP8, posição 0: chegadas `+1` têm medianas **11350** e **11360**; chegadas `−1` têm **11981** e **11880**. A diferença entre as medianas dos dois grupos é `|11355 − 11930,5| = 575,5`, com denominador relativo à média `((11355 + 11930,5)/2)`, resultando em **4,94%**.

A diferença `575,5` **não confirma** a hipótese exata de aproximadamente `550` pontos e `4,7%`: é `25,5` pontos maior e `0,24` ponto percentual maior. Ela também não deve ser lida como medição mecânica de histerese. As amostras individuais que a sustentam são heterogêneas: em posição 0, a chegada `+1` usa valores na faixa `11318–11408`/`11353–11405`, enquanto a chegada `−1` usa `11846–11981`/`11833–11952`; mean permanece `114`, mas não há snapshot atômico de exposição nem frame identity.

Comparações adicionais importantes:

- `357` é menor que o maior efeito de visita observado em EP7 (`575`) e EP8 (`520`), e menor que a diferença direcional agregada de EP8 (`575,5`).
- `357` é maior que o maior MAD intravisita dos quatro episódios (`119`) e maior que a diferença direcional agregada de EP2 (`282`) e EP7 (`190`).
- A mesma margem representa frações muito diferentes dos scores de referência: `1,61%`, `0,77%`, `2,99%` e `3,02%`. Portanto, não se deve reajustá-la a partir de EP7/EP8.

## 5. EP8: replay fiel, parada antecipada e contrafactual

A análise B/C/D identifica o candidato no **comando índice 3**, com decisão no timestamp **358193**. O comando índice 3 já foi executado na trajetória registrada; o seu último resultado observado é a amostra que fecha a decisão. O primeiro comando que seria evitado/alterado é o **índice 4**, timestamp de comando **358506**, direção `−1`, step `3`.

O bracket lógico usado é `−3 → 0 → +3 → 0`, todo ele já presente no log antes do comando índice 4. Para tornar explícita a diferença entre dados brutos e a janela efetivamente agregada, a tabela inclui todas as amostras da janela e marca as amostras descartadas pelo agregador de três amostras:

| Visita/comando | Posição lógica | Timestamp | Score | Uso na mediana temporal |
|---|---:|---:|---:|---|
| índice 0 | −3 | 355729 | 8056 | usado |
| índice 0 | −3 | 355861 | 8103 | usado |
| índice 0 | −3 | 355978 | 8059 | usado |
| índice 1 | 0 | 356439 | 11371 | descartada pelo tail-3 |
| índice 1 | 0 | 356544 | 11350 | usada |
| índice 1 | 0 | 356668 | 11318 | usada |
| índice 1 | 0 | 356768 | 11408 | usada |
| índice 2 | +3 | 357169 | 10608 | usada |
| índice 2 | +3 | 357298 | 10037 | usada |
| índice 2 | +3 | 357399 | 10081 | usada |
| índice 3 | 0 | 357863 | 11875 | descartada pelo tail-3 |
| índice 3 | 0 | 357978 | 11981 | usada |
| índice 3 | 0 | 358085 | 11981 | usada |
| índice 3 | 0 | **358193** | **11846** | usada; decisão |

As medianas temporais usadas pelo candidato são `8059` em `−3`, `10081` em `+3`, `11350` na primeira visita a `0` e `11981` na segunda visita a `0`; o score central agregado é `11665,5`, com margem `11665,5 − max(8059,10081) = 1584,5`, acima de 357. As duas primeiras amostras de posição 0 (`356439` e `357863`) continuam registradas e são explicitamente identificadas como não usadas pelo tail-3; não são substituídas nem apagadas.

Classificação metodológica:

1. **Até o índice 3:** replay fiel; não há movimento ou score inventado.
2. **A decisão:** classificação host-only calculada sobre observações já existentes, com parada antecipada simulada.
3. **A partir do índice 4:** contrafactual. O log real executou o comando índice 4 e os seguintes; não se afirma qual seria a exposição, a trajetória, o resultado terminal ou os scores se o comando não tivesse sido enviado.

Assim, a coluna `contrafactual` do harness significa que o resultado antecipado mudaria a trajetória, não que as amostras até `358193` sejam contrafactuais. A análise não inventa scores futuros.

## 6. Contagem real de evidências HOLD

Com a calibração congelada e o bracket temporal exigido:

- EP2: `INSUFFICIENT_DATA` — não há suporte suficiente para um bracket completo não-AUTO.
- EP5: `HOLD_CANDIDATE`, replay fiel — candidato observado no caminho registrado.
- EP7: `HOLD_CANDIDATE`, replay fiel — candidato observado no caminho registrado.
- EP8: `HOLD_CANDIDATE` calculado em replay fiel com parada antecipada; consequência operacional após o índice 4 é contrafactual.

A contagem conservadora solicitada é **candidato sustentado por 2 casos nesta sessão**: EP5 e EP7. EP8 é evidência adicional de consistência do classificador host-only, mas não aumenta a contagem de casos operacionalmente sustentados enquanto a parada antecipada não for observada em trajetória real. Nunca chamar esses casos de `HOLD validado` ou `CONVERGED` de produção.

## 7. Estratégias e adversários host-only

B exige vizinhos equidistantes, centro efetivamente alcançado, três amostras válidas e margem superior à calibração. C usa o mesmo agregador robusto e só pode excluir a primeira amostra com pelo menos quatro amostras. D calcula uma parábola apenas para classificar HOLD; não emite comando fracionário.

| Estratégia / EP | Comandos até decisão | Tempo | Margem até timeout | Candidato | Resultado host |
|---|---:|---:|---:|---:|---|
| A / 2 | 7 | 4985 ms | 3015 ms | −3 | `SEARCH_LIMIT` registrado |
| B/C/D / 2 | 7 | 4985 ms | 3015 ms | — | `INSUFFICIENT_DATA` |
| A / 5 | 8 | 6070 ms | 1930 ms | 6 | `CONVERGED` registrado |
| B/C/D / 5 | 8 | 6070 ms | 1930 ms | 6 | `HOLD_CANDIDATE`, replay fiel |
| A / 7 | 8 | 5752 ms | 2248 ms | 0 | `SEARCH_LIMIT` registrado |
| B/C/D / 7 | 8 | 5752 ms | 2248 ms | 0 | `HOLD_CANDIDATE`, replay fiel |
| A / 8 | 8 | 5889 ms | 2111 ms | 0 | `BLOCKED` registrado |
| B/C/D / 8 | 4 | 2903 ms | 5097 ms | 0 | `HOLD_CANDIDATE`; replay fiel até cmd 3, consequência após cmd 4 contrafactual |

Todos ficaram abaixo do teto estrutural de 24 comandos e não adicionaram comandos. Nos adversários sintéticos definidos pelo harness — pico falso isolado, platô, ruído 3×, baixa textura, ambiente variável, histerese acima da margem, métricas duplicadas, transiente pós-movimento e retorno inconsistente — foram observados zero falsos `CONVERGED` e zero violações de budget. Isso é propriedade dos cenários definidos, não garantia universal da câmera.

## 8. AUTO, budgets e limites de inferência

A máquina AUTO requisita first move, first return, opposite move e opposite return (`src/smoothaf_monitor.h:591-629`). Cada comando marca `awaiting_result` (`src/smoothaf_monitor.h:1262-1279`); antes do processamento AUTO, `smoothaf_episode_prepare_score()` limpa a marca e atualiza score/posição (`src/smoothaf.c:2159`; `src/smoothaf_episode.h:182-196`).

`finish_auto_probe()` restaura deliberadamente a origem e o score inicial (`src/smoothaf_episode.h:61-72`). Isso descarta evidência direcional do probe para fins de seleção persistente por desenho, não por erro comprovado. O limite de sessão inclui limite configurado, AUTO e eventual recovery, com teto estrutural 24 (`src/smoothaf_monitor.h:552-563`); EP8 registra oito comandos, sem recovery e sem timeout adicional.

## 9. Auditoria de metadados Legacy

| Campo investigado | Classificação | Evidência de fonte | Consequência para B2-B |
|---|---|---|---|
| Frame ID/contador associado ao YUV medido | **NÃO ENCONTRADO** | `struct vram_info` contém somente `vram`, `width`, `pitch`, `height` (`docs/references/magic-lantern-branch-lua_fix/src/vram.h:56-62`). `get_yuv422_vram()` seleciona buffers LiveView e retorna a estrutura (`src/vram.c:521-559`), sem contador. SmoothAF lê o buffer diretamente (`src/smoothaf.c:1483-1489`). | Timestamps distintos e métricas idênticas não provam frames distintos. Não substituir frame ID ausente por timestamp. |
| Exposição efetiva no frame LiveView/REC | **DISPONÍVEL COM LIMITAÇÕES** | `lens_info` expõe `raw_shutter`, `raw_iso` e `raw_iso_auto` (`docs/references/magic-lantern-branch-lua_fix/src/lens.h:67-72`); handlers atualizam os valores a partir de propriedades (`lens.c:1338-1350`, `:1389-1453`). | São estados/propriedades observados, não snapshot atômico associado ao YUV medido. O fluxo atual mede e só depois obtém `completed` (`src/smoothaf.c:2002-2007`); exposição efetiva por frame permanece não comprovada. |
| Timestamp do frame, distinto da conclusão da medição | **NÃO ENCONTRADO** | A medição começa, lê YUV e termina; `completed = get_ms_clock()` ocorre após `smoothaf_measure()` (`src/smoothaf.c:2002-2007`). O `MEASUREMENT` é registrado com `completed` (`src/smoothaf.c:2019-2026`). | O timestamp do log é timestamp de conclusão da medição, não timestamp de captura do frame. |
| `lens_info.focus_pos` | **DISPONÍVEL COM LIMITAÇÕES** | É declarado como contador de fine steps relativo, iniciado em zero e dependente da lente (`docs/references/magic-lantern-branch-lua_fix/src/lens.h:81-83`); a API Lua documenta perda de rastreamento durante MF (`modules/lua/lua_lens.c:29-35`); o handler copia campo model-dependent de `PROP_LV_LENS` (`lens.c:1708-1721`). | Pode ser registrado como diagnóstico relativo, mas não é encoder absoluto nem prova de deslocamento óptico. Não usar automaticamente como proxy de frame, distância ou histerese. |

### Instrumentação mínima futura

Sem implementar nesta B2-B, a investigação de F4/F5/F8/F9/F10 deveria acrescentar, na origem da amostra, uma identidade/contador de captura ou vínculo explícito ao buffer e um timestamp de captura; capturar junto o snapshot de shutter/ISO efetivamente associado; registrar `focus_pos` antes/depois apenas como diagnóstico; e manter `score`, `mean`, validade, confiança, comando, direção, step, posição lógica e estado do gate no mesmo contexto de amostra. A instrumentação precisa permitir distinguir:

- mesmo frame relido versus frames consecutivos com métricas iguais (F9);
- primeira amostra após movimento versus amostra estabilizada (F4/F10);
- mudança de exposição/ambiente versus resposta de foco (F5/F8);
- posição lógica, `focus_pos` relativo e movimento óptico independente.

Para B2-C2, a opção compatível é um **evento persistente aditivo e versionado**, sem alterar os seis payloads existentes, as nove colunas TSV, o ring fixo ou o FIO assíncrono. O evento não deve guardar texto variável nem fazer I/O no produtor.

## 10. Limitação de cena única e D1/D2

EP2, EP5, EP7 e EP8 pertencem à mesma sessão **SMTH003**, com a mesma configuração experimental registrada. Eles não demonstram generalização para outras cenas, distâncias, texturas, contrastes, lentes ou condições de iluminação.

D1 estacionário em outras cenas pode medir dispersão temporal, transientes e estabilidade de luminância. Ele **não** mede a diferença entre visitas depois de movimentos, nem isola backlash/histerese. Essa segunda questão exige D2 ou outro experimento físico autorizado. Não serão propostos novos episódios físicos de ACTIVE CONTINUOUS nesta etapa.

## 11. Proposta B2-C

### B2-C1 — validação host-only

**Escopo:** ampliar o harness puro, sem tocar no controlador de produção.

- comparar margens absoluta, relativa e combinada em episódios congelados;
- testar agregação temporal, exclusão condicional da primeira amostra, retornos e direções;
- adicionar adversários de baixa textura, ambiente variável, histerese, duplicação de frame e retorno inconsistente;
- manter bracket completo, `MIN_TEMPORAL_SAMPLES=3`, validade/confiança, ambiente válido e `INSUFFICIENT_DATA` por padrão;
- não emitir step fracionário, não aumentar budget, não relaxar gates, timeout, STOP/PANIC ou recovery.

**Impacto:** somente ferramenta, testes e relatório host-only. **Riscos:** falso conforto por uma única sessão e por proxies temporais. **Aceitação:** zero falsos `CONVERGED` nos adversários definidos, zero violações de budget e rastreabilidade de cada amostra usada.

### B2-C2 — decisão HOLD em shadow mode

**Escopo:** somente registrar a decisão que o classificador tomaria, sem alterar comandos, gates, budgets, estados ou resultados operacionais. Não implementar nesta B2-B.

Contrato proposto para um futuro evento, por exemplo `HOLD_SHADOW_DECISION`, com **schema version explícito** e seis slots `int32_t` já existentes:

| Payload | Conteúdo proposto |
|---|---|
| `[0]` | `schema_version << 16 | origin`; origin identifica ponto do classificador, não um novo comando |
| `[1]` | resultado: `INSUFFICIENT_DATA`, `HOLD_CANDIDATE` ou rejeição; reservar bits para flags `environmental`, `invalid`, `no_frame_id` |
| `[2]` | índice do comando/ponto lógico avaliado e contagem de observações; valor unsigned empacotado |
| `[3]` | posição lógica candidata e posição lógica efetivamente registrada no momento, em dois `int16_t` empacotados |
| `[4]` | margem robusta assinada (`center − max(neighbors)`) |
| `[5]` | suporte, motivo, direção/consistência e classificação `REPLAY`/`COUNTERFACTUAL`, em bitfields versionados |

Semântica e limites:

1. Registrar no máximo um evento por bracket concluído e um evento terminal por episódio; se o classificador reavaliar o mesmo ponto, aplicar deduplicação por `(episode, command index, schema)`.
2. Registrar `INSUFFICIENT_DATA` explicitamente quando faltar frame identity, exposição associada, suporte temporal, validade/confiança, bracket ou ambiente válido; não preencher ausências com proxies silenciosos.
3. Incluir o ponto atual da trajetória realmente executada para permitir comparar a decisão shadow com o caminho que a câmera de fato percorreu.
4. O evento é aditivo/versionado: mantém record size de 36 bytes, seis payloads, nove colunas TSV, ring fixo e produção não bloqueante; FIO permanece somente no consumidor assíncrono.
5. O resultado shadow nunca chama `lens_focus()`, nunca muda `awaiting_result`, nunca converte `HOLD_CANDIDATE` em `CONVERGED` e nunca cancela/insere comando.

**Aceitação B2-C2:** eventos decodificáveis por versão, limite de frequência respeitado, zero alteração de trajetória e comparação posterior entre decisão shadow e trajetória efetivamente executada. A comparação não transforma replay em validação física.

### D2 — calibração física posterior

D2 é separado de B2-C1/B2-C2 e requer autorização independente. Deve investigar steps solicitados `1`, `2` e `3`, ambas as direções, repetições limitadas, retorno API, duração, `focus_pos` relativo antes/depois, score/mean, exposição/frame metadata e evidência óptica independente. Deve preservar STOP/PANIC, gates, cooldown, timeout e budgets. Não foi executado.

**Integração operacional:** somente depois de B2-C1, B2-C2 e D2 produzirem evidência adicional, em cenas independentes e com critérios de aceitação aprovados. Esta proposta não é autorização de implementação.

## 12. Integridade e verificação

Durante B2-B não foram modificados arquivos de produção, não houve build ARM, instalação em SD, ACTIVE CONTINUOUS, D2, teste físico, commit, push, merge, reset ou rebase. O build solicitado é `-TestOnly`, que executa a verificação em ambiente de testes e não gera módulo ARM.

A verificação final deve confirmar:

- `./build-smoothaf.ps1 -TestOnly` concluído;
- testes B2-B concluídos (`python -m unittest tests/test_b2b_host_analysis.py` ou comando equivalente do build);
- `git diff --check` sem erros;
- nenhuma alteração em `src/`;
- somente estes três arquivos host-only pendentes:
  - `docs/B2-B.md`;
  - `tools/b2b_host_analysis.py`;
  - `tests/test_b2b_host_analysis.py`.

**Commit exclusivo proposto, não executado:**

```text
B2-B: consolidate host-only convergence evidence and HOLD limits
```

A autorização do usuário continua necessária antes de executar esse commit.
