# SmoothAF V2 — B2-C1b: robustez host-only de `best`, picos e repetibilidade

**Status:** concluída como análise host-only para revisão; sem integração de HOLD, sem alteração de produção, sem alteração de `src/`, sem geração ARM, testes físicos ou ACTIVE CONTINUOUS.

## 1. Escopo e reprodução

A B2-C1b reutiliza `tools/b2c1a_canonical_audit.py` como parser e fonte única da reconstrução do log. `tools/b2c1b_host_analysis.py` consome `parse_log`, `canonical_rows`, `visits`, `policy_summary`, `pair_comparisons` e `replay_ep8`; não duplica a interpretação TSV, a reconstrução de episódios ou a semântica de posições.

Fonte:

- `logs/SMTH003.LOG`
- SHA-256: `E08D0D90E6BA6791CDC36A5C5CFBE614FCDAD1AF9614B240151132D4448B36E7`
- 367 linhas canônicas
- zero erros de parsing
- frame ID: ausente no log

Reprodução:

```powershell
python -B tools/b2c1b_host_analysis.py `
  --log logs/SMTH003.LOG `
  --json artifacts/B2-C1b.json `
  --csv artifacts/B2-C1b.robustness.csv
```

Os artefatos em `artifacts/` são locais, ignorados e reproduzíveis. Nenhum é incluído no commit desta etapa.

## 2. Definições

- **`strict_max`:** máximo individual de scores válidos/confidentes, mantendo a semântica de produção de `best`. Não substitui o `best` de produção.
- **`tail3_median`:** mediana dos três últimos scores válidos/confidentes de cada visita; requer três amostras.
- **`complete_median`:** mediana de todos os scores válidos/confidentes da visita; requer três amostras.
- **`trimmed_mean`:** média após remover um valor de cada extremidade; requer pelo menos cinco amostras. Não houve visitas elegíveis no log canônico, portanto os resultados são `INSUFFICIENT_EVIDENCE`.
- Para múltiplas visitas na mesma posição, a análise host-only usa a mediana das estimativas de visita. Isso é um resumo exploratório, não votação nem evidência independente.
- Tuplas métricas repetidas são marcadas pelo parser canônico. Timestamps distintos não são tratados como frames independentes.

A margem `357` permanece somente como fórmula reproduzível da B2-B e **não calibrada**. Para uma margem geral de HOLD, a conclusão continua **`INSUFFICIENT_EVIDENCE`**. A B2-C1b não recalibra esse valor.

## 3. Comparação numérica dos estimadores

Os vencedores por episódio e política são:

| Episódio | `strict_max` | `tail3_median` | `complete_median` | `trimmed_mean` |
|---:|---:|---:|---:|---:|
| 2 | −3 / 22187 | −3 / 21198,0 | −3 / 21198,0 | INSUFFICIENT_EVIDENCE |
| 3 | +3 / 14229 | 0 / 1883,0 | 0 / 1883,0 | INSUFFICIENT_EVIDENCE |
| 4 | −3 / 15784 | −3 / 15516,0 | −3 / 15516,0 | INSUFFICIENT_EVIDENCE |
| 5 | +6 / 46476 | +6 / 46235,5 | +6 / 46219,25 | INSUFFICIENT_EVIDENCE |
| 6 | −3 / 17093 | −3 / 16190,0 | −3 / 16190,0 | INSUFFICIENT_EVIDENCE |
| 7 | 0 / 11947 | 0 / 11582,5 | 0 / 11582,5 | INSUFFICIENT_EVIDENCE |
| 8 | 0 / 11981 | 0 / 11620,0 | 0 / 11618,25 | INSUFFICIENT_EVIDENCE |
| 9 | +3 / 20910 | +3 / 10316,0 | +3 / 10316,0 | INSUFFICIENT_EVIDENCE |

Formato: `posição / valor`. Os valores são estatísticas host-only; não são novos scores de produção.

### Divergências relevantes

- EP3: `strict_max` escolhe +3, enquanto as medianas escolhem 0.
- EP9: `strict_max` escolhe +3, enquanto as medianas mantêm +3, mas o máximo `20910` é muito maior que a referência mediana `10316`.
- EP2, EP4 e EP6 mantêm a posição vencedora entre máximo e medianas, mas continuam sujeitos à limitação de uma sessão e ausência de frame ID.
- EP5, EP7 e EP8 mantêm a posição vencedora, porém isso é concordância entre estimadores derivados das mesmas observações, não evidência independente.
- Nenhuma visita alcança o mínimo de cinco amostras para média aparada.

## 4. Sensibilidade a outliers, primeira medição e duplicatas

Foi calculado leave-one-out para cada medição, além de remoção da primeira medição de cada visita e remoção de tuplas métricas repetidas.

A remoção de uma única amostra altera o vencedor da posição em:

| Episódio | Políticas sensíveis |
|---:|---|
| 2 | `tail3_median`, `complete_median` |
| 3 | `tail3_median`, `complete_median` |
| 4 | `tail3_median`, `complete_median` |
| 5 | nenhuma das políticas reportadas |
| 6 | `tail3_median`, `complete_median` |
| 7 | nenhuma das políticas reportadas |
| 8 | nenhuma das políticas reportadas |
| 9 | `tail3_median`, `complete_median` |

A tabela significa sensibilidade a pelo menos uma remoção no conjunto leave-one-out; não significa que todos os pontos sejam outliers. Os detalhes por linha, score e posição estão em `artifacts/B2-C1b.json`.

A remoção da primeira medição pós-movimento e a remoção de tuplas repetidas também são emitidas por política no JSON. Elas não transformam uma medição em frame independente nem autorizam descartar registros da fonte canônica.

Conclusão: `best` pode ser dominado por uma única medição em alguns episódios; as medianas são úteis como análise de sensibilidade, mas também não são validadas como substitutas operacionais.

## 5. Repetibilidade entre visitas

As regressões obrigatórias foram preservadas:

- **EP7, posição 0: `575`**
- **EP8, posição 0: `520`**

Ambos são definidos exatamente como **diferença absoluta entre medianas `tail-3` de visitas não-AUTO na mesma posição lógica**. A definição e os dados por visita estão em `artifacts/B2-C1b.json` e no CSV derivado.

Esses números são `REPEATABILITY_PROXY`, não medição de posição óptica, histerese mecânica ou repetibilidade física confirmada. Três timestamps distintos não comprovam três frames independentes. A fonte não possui frame ID, snapshot de exposição ou captura atômica associada ao registro.

Comparações incompletas, inclusive visitas com menos de três timestamps distintos, permanecem `INSUFFICIENT_EVIDENCE`.

## 6. Discriminação de picos

A análise separa:

1. **Variação temporal na mesma visita:** amplitude, MAD, máximo e medianas das amostras da visita.
2. **Variação entre visitas na mesma posição:** diferenças entre estimativas de visita, incluindo os proxies 575 e 520.
3. **Diferença entre posições lógicas vizinhas:** `peak_prominence = score_central - max(score_esquerdo, score_direito)` quando ambas as vizinhas existem e têm evidência suficiente.

O status `PEAK_CANDIDATE` nesta seção é uma observação host-only de predominância sobre vizinhos; não é threshold de HOLD, não usa votação para calibrar controle e não implica causalidade física.

### Resultados com `complete_median`

| Episódio | Posição | Prominência | Status |
|---:|---:|---:|---|
| 5 | +6 | 21712,25 | PEAK_CANDIDATE |
| 7 | 0 | 1479,00 | PEAK_CANDIDATE |
| 8 | 0 | 1525,25 | PEAK_CANDIDATE |
| Demais posições/episódios | — | — | INSUFFICIENT_EVIDENCE |

EP2, por exemplo, não oferece vizinhança suficiente para uma discriminação espacial completa. A matriz integral, incluindo posições sem vizinho e todas as políticas, está no JSON.

Os candidatos EP7 e EP8 não devem ser interpretados como validação do replay de parada antecipada. O replay continua sendo `REPLAY_FIEL_COM_PARADA_ANTECIPADA` para a oportunidade causal após o comando 3, com resultado operacional não testado.

### Casos sintéticos

Foram incluídos casos determinísticos com seeds registradas:

- `well_defined_peak`: identificado como candidato;
- `plateau`: não discriminado;
- `outlier_isolated`: não discriminado quando a proeminência é apenas uma unidade;
- `low_confidence`: `INSUFFICIENT_EVIDENCE` por amostras insuficientes;
- `missing_neighbor`: sem evidência de pico por ausência de vizinho.

As seeds são `synthetic_cases=261` e `property_samples=262`. Os casos sintéticos são testes de comportamento do analisador, não dados físicos.

## 7. Testes de propriedades e mutação

Os testes implementados em `tests/test_b2c1b_host_analysis.py` verificam:

- **Causalidade:** o replay do prefixo EP8 até a linha 321 mantém classificação, linha de decisão e último comando; o futuro não altera a decisão anterior.
- **Duplicação:** copiar uma observação não cria suporte de independência de frame.
- **Espelhamento:** inverter posições e direções espelha o vencedor estatístico.
- **Escala:** multiplicar scores por dois escala valores absolutos e preserva a relação proporcional.
- **Leave-one-out:** mudanças de vencedor são explicitamente reportadas.
- **Mutação:** sentinelas para usar a janela errada do `tail-3`, não inverter posição e não escalar valor absoluto são detectadas (`killed=true`).
- **Casos positivos/negativos:** pico definido, platô, outlier isolado, baixa confiança e vizinho ausente.

Os testes não introduzem nova métrica de imagem e não usam resultados futuros de episódios em decisões anteriores.

## 8. Validação executada

Passaram:

```text
python -B tests/test_b2c1b_host_analysis.py   # 7 testes
python -B tests/test_b2c1a_canonical_audit.py # 8 testes
python -B tests/test_b2b_host_analysis.py     # 4 testes
```

A geração dos artefatos terminou sem erros de parsing e reproduziu EP7 `575` e EP8 `520`. A suíte host completa foi executada e passou. Esta etapa ainda não criou commit nem fez push.

## 9. Impacto potencial sobre HOLD

### Acquisition HOLD

A análise sugere que um máximo individual pode depender de uma única amostra e que a posição pode divergir entre máximo e estimadores temporais, especialmente EP3. Isso é relevante para aquisição porque um pico isolado não deve, por si só, ser transformado em confirmação de foco. Porém, não há base host-only suficiente para escolher margem, número de amostras ou regra de retenção operacional.

**Impacto:** informativo; nenhum GO para integração.

### Tracking HOLD

EP7 e EP8 mostram diferenças de 575 e 520 entre visitas na mesma posição, e há candidatos de pico host-only em 0. Isso pode influenciar uma futura política de estabilidade, mas não distingue variação óptica, exposição, frame reutilizado, movimento mecânico ou estado ambiental. Não valida histerese nem um HOLD de tracking.

**Impacto:** informativo; nenhum GO para integração.

## 10. Menor próximo experimento

O menor experimento recomendado é uma coleta física controlada, ainda separada de qualquer HOLD:

1. manter a lente imóvel em três posições lógicas conhecidas, incluindo centro e vizinhas;
2. registrar frame ID ou contador monotônico de captura junto a cada medição;
3. registrar exposição, ISO/ganho, mean, SAD, Laplacian e flags de validade/confiança;
4. repetir cada posição em pelo menos três visitas independentes, com ordem randomizada e sem AUTO entre comparações;
5. repetir o ciclo após retorno mecânico à posição de origem;
6. não usar os dados para controlar a lente até uma revisão específica.

Esse experimento tem o menor escopo capaz de separar repetição de registro, variação temporal e possível efeito de posição. Não é autorizado nesta etapa B2-C1b.

## 11. Decisão

**B2-C1b host-only: concluída para revisão.**

**GO:** revisar robustez estatística e planejar o experimento mínimo de aquisição de evidência.

**NO-GO:** desenvolver B2-C1c ou B2-C2 como HOLD operacional neste momento. A evidência host-only não valida threshold, regra de `best`, Acquisition HOLD, Tracking HOLD ou histerese mecânica.

Qualquer próxima etapa deve preservar:

- margem `357` como não calibrada;
- EP7 `575` e EP8 `520` com a definição exata;
- EP8 como oportunidade causal de replay, não resultado operacional observado;
- ausência de frame ID como limitação explícita;
- nenhuma alteração de `src/`, gates, budgets, STOP/PANIC, recovery ou release.
