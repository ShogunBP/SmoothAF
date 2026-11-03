# SmoothAF — Caderno cumulativo de pesquisa e implementação

Atualizado em 2026-11-02. Documento de trabalho para receber novos artigos; NÃO representa código já implementado nem validação em hardware.

## 0. Ambiente e objetivo
- Canon EOS 600D / T3i, firmware 1.0.2; Magic Lantern Legacy Lua Fix; EF 50 mm f/1.8 STM; DIGIC 4.
- Projeto SmoothAF. Meta: foco por contraste, contínuo, durante gravação Movie, controlado e observável, sem hunting desnecessário.
- Implementação relatada até aqui: LiveView YUV422 (luminância), ROI central ~64x64, métricas SAD e Laplaciano, monitor de POSSIBLE_BLUR, máquina de estados de episódios, medidas pré/pós-movimento, STOP/PANIC e DRY RUN. ACTIVE CONTINUOUS/Movie AF está em desenvolvimento pelo agente; NÃO assumir implementado ou testado.
- Método de validação planejado: gravação na T3i com celular filmando a tela/telemetria; confirmar se menu/overlay permanece visível durante Movie recording antes de depender dessa abordagem.

## 1. Fonte A — comparação de 15 métricas (2021)
**Referência:** Marina Bonet Sanz, Felipe Machado Sánchez e Susana Borromeo. *An algorithm selection methodology for automated focusing in optical microscopy*. First published 25 Dec 2021. DOI: https://doi.org/10.1002/jemt.24035
**Dados/implementação indicados pelos autores:** https://github.com/aquimb/Autofocus-URJC (não inspecionado neste trabalho).
**Escopo:** 150 stacks de tecidos em microscopia, 5x/10x, 8/16 bits; 15 funções de foco; quatro critérios (accuracy, range, false maxima, full width at 50% maximum). Não transferir ranking diretamente para vídeo da T3i.
**Descoberta:** Squared Gradient liderou rankings globais reportados, com Brenner e Thresholded Absolute Gradient entre os bons candidatos. O artigo não incorporou custo de execução à avaliação, mas esse custo é essencial para DIGIC 4.

### 1.1 Fórmulas fornecidas pelo usuário como imagens da publicação
Notação: g(i,j) luminância no pixel (i,j); theta = threshold da publicação; somar apenas onde vizinhos exigidos estejam dentro da ROI. Implementação C deve acumular em 64 bits ou demonstrar ausência de overflow. Fórmulas abaixo registram conceito e condições; checar limites exatos dos somatórios contra as imagens se reproduzir formalmente no paper.

**Squared Gradient (gradiente quadrático horizontal de deslocamento 1)**
```
SG = SUM_{i,j} [g(i,j+1) - g(i,j)]^2
incluir termo somente se |g(i,j+1) - g(i,j)| >= theta
```

**Brenner Gradient (gradiente horizontal de deslocamento 2)**
```
BR = SUM_{i,j} [g(i,j+2) - g(i,j)]^2
incluir termo somente se |g(i,j+2) - g(i,j)| >= theta
```

**Tenengrad Gradient**
```
TEN = SUM_{i,j} [Gx(i,j)^2 + Gy(i,j)^2]
Gx, Gy = derivadas obtidas por Sobel horizontal e vertical, conforme paper
```

**Energy of Image Laplacian (5-pontos)**
```
L5 = g(i,j+1) + g(i,j-1) + g(i+1,j) + g(i-1,j) - 4*g(i,j)
LAPL5 = SUM_{i,j} [L5]^2
```
Nota importante: quadrado incide sobre toda a resposta local de Laplace.

**Comparações desejadas (sem controlar a lente):** calcular as quatro métricas sobre o mesmo frame e a mesma ROI; comparar curvas normalizadas por posição de foco; medir falso pico, localização do máximo, estabilidade temporal, custo CPU/latência; testar theta em valores adequados ao ruído/iluminação da T3i (não transplantar 20% do estudo automaticamente).

## 2. Fonte B — contagem de inversões de inclinação (2025)
**Referência:** Jing-Feng Weng. *Advanced autofocus algorithm for microscopy imaging: counting-based identification of high-intensity, low-intensity, overexposed, and dark regions in metallic samples.* Optics Express 33(8), 17705+, publicado 14 abr 2025. DOI: https://doi.org/10.1364/OE.561783
**Origem no chat:** `Advanced_autofocus_algorithm_for_microscopy_imagin.pdf` (16 páginas).
**Escopo:** microscopia, amostras metálicas, iluminação alta / baixa / superexposta e regiões escuras; experimentos em stacks de centenas de posições com estágio piezo; cálculos demonstrados no MATLAB/PC. NÃO há evidência direta de desempenho em Canon DIGIC 4/STM, Movie Mode ou sujeitos em movimento.

### 2.1 Método proposto — equações 2–5, páginas 4–5
A publicação define sinais de gradientes nos eixos horizontal e vertical de uma máscara Nx x Ny centrada em uma ROI. Representação operacional equivalente da parte de contagem (precisa tratar diferenças zero explicitamente):
```
d_row(i,j,z) = I(i,j,z) - I(i-1,j,z)
d_col(i,j,z) = I(i,j,z) - I(i,j-1,z)
sgn(d) = d/abs(d)    para d != 0   [paper; zero não definido nessa razão]

flip_row(i,j,z) = 1 se sgn(d_row(i,j,z))*sgn(d_row(i-1,j,z)) == -1; senão 0
flip_col(i,j,z) = 1 se sgn(d_col(i,j,z))*sgn(d_col(i,j-1,z)) == -1; senão 0

S(i,j,z) = -1 se ocorre pelo menos uma das condições de inversão; senão 0
C_total(z) = SUM_{ROI} abs(S(i,j,z))
C_total(z) = C_bg(z) + C_sample(z)   [decomposição interpretativa do paper]
```
**Atenção:** equação 3 usa condições alternativas (OR): a contagem por pixel é no máximo 1, NÃO soma dupla dos flips. Convenção do primeiro pixel do paper: ambos sinais começam em +1. A razão d/abs(d) é indefinida para d=0; numa implementação de benchmark, explicitar uma política para zero e testar se ela altera resultados. As derivadas e flip exigem bordas/vizinhos válidos.
**Comportamento observado no artigo:** relação entre C_bg e C_sample ajuda a distinguir condições de luminosidade; regiões escuras têm curvas irregulares e podem ser identificadas como de confiabilidade limitada. O paper usa ajuste polinomial ao longo de posições de foco: NÃO assumir que uma única contagem em um frame já classifica a exposição ou determina sentido de movimento.

### 2.2 Números comparativos — Tabela 1, página 7
Foco de referência da amostra: frame 192; máscara 41x41, stacks de 400 frames.

| Método | Alta luz sem clipping | Baixa luz | Superexposto |
|---|---:|---:|---:|
| Contagem de inversões proposta | 192 | 192 | 192 |
| Squared Gradient | 192 | 192 | 188 |
| Brenner Gradient | 192 | 192 | 188 |
| Energy Laplacian (kernel de 2025) | 192 | 195 | 189 |
| Image Power | 164 | 179 | 87 |
| Intensidade máxima | 124 | 101 | 399 |

O paper reporta ~0,3 s para método proposto sobre stack de 400 imagens vs ~0,1 s para cada método controle no PC de teste; isso NÃO é latência por frame no DIGIC 4 e inclui processamento de stack/ajuste segundo descrição.

**Diferença entre os papers:** Laplaciano de 2025 usa kernel 3x3 de alta ordem:
```
[-1, -4, -1]
[-4, 20, -4]
[-1, -4, -1]
EnergyLaplace2025 = SUM (convolution(image, kernel))^2
```
Não confundir com Laplaciano 5-pontos da fonte A ou presumir que o SmoothAF atual implementa qualquer um deles exatamente. Comparar kernels lado a lado.

### 2.3 Adaptação candidata (NÃO implementada)
1. Fazer benchmark passivo de `C_total` junto com Squared Gradient, Brenner, Laplaciano 5 pontos e Tenengrad, sem comandar lente.
2. Registrar, no mesmo frame/ROI, saturação de pixels, nível médio de luminância, variação de exposição, validade da imagem e tempo de processamento. Separar ruído/flicker de mudança de foco.
3. Testar a contagem de inversões com pixels de mesma luminância, ruído, detalhes repetitivos, textura escassa, iluminação baixa, clipping e mudança de exposição durante vídeo.
4. Não classificar `dark/low/overexposed` usando um único limiar inventado; a análise publicada depende da forma das curvas ao longo de uma stack. Inicialmente usar a métrica como sinal comparativo, não como decisão autônoma de AF.
5. Se o custo computacional e a estabilidade forem bons, estudar fusão de qualidade/confiança da medição com as métricas convencionais. A fusão requer validação empírica na T3i.

## 3. Roadmap proposto — distinguir o que existe do que será feito
**Etapa A — EM ANDAMENTO por agente externo:** ACTIVE CONTINUOUS e testes de Movie Dry Run/Active, com gate de gravação auditado. Sem limite global acumulado fixo de 4 movimentos, mas com limites por episódio, tempo, cooldown, falhas, STOP e PANIC. Confirmar viabilidade e segurança de chamadas STM durante gravação antes de habilitar comandos.
**Etapa B — COLETAR LINHA DE BASE:** medir tempo para adquirir foco de muito longe/perto, pequenos ajustes, estabilidade já focada, hunting, movimento do sujeito, pouca luz e overexposição. Usar vídeo da câmera + celular na tela se menu visível; registrar condições, passos, tempo, falhas e duração.
**Etapa C — METRIC BENCHMARK sem controle da lente:** SG, Brenner, Tenengrad, Laplace 5 pontos, kernel Laplace 2025, C_total; todos sobre mesma captura Y/ROI. Instrumentar custo, scores normalizados, consistência e confiabilidade; cuidado com intervalos temporais e overflow.
**Etapa D — CURVAS CONTROLADAS:** teste em posições de foco conhecidas ou aproximações por passos STM, com sujeito estático; considerar comparação com foco indicado manualmente; avaliar erro do pico, false maxima, monotonicidade e largura a meia altura, mais custo por frame e falhas sob exposição variável.
**Etapa E — ESCOLHA/COMBINAÇÃO:** somente trocar a métrica que controla a lente depois de resultados reais demonstráveis, preservar rollback e controles de segurança. A escolha pode diferir entre busca de foco e manutenção de foco.

## 4. Pendências e cautelas
- O próximo artigo anunciado pelo usuário será sobre autofocus para câmera (mais relevante externamente); adicionar aqui e reavaliar prioridades antes de implementar novas métricas.
- Confirmar a ROI realmente acessível ao pipeline Movie, tempo de amostragem, YUV422 UYVY e custo ARM; sem supor equivalência com experimentos microscópicos.
- O ROI mede contraste: mudança de composição/exposição/objeto pode alterar score independentemente do foco. Não presumir que score maior sempre representa sujeito correto.
- Controlar `lens_focus()` com feedback de imagem validado, tempo de estabilização, STOP/PANIC; não confiar só no retorno positivo da API.
- FIO/SD export e Canon AF Assist ficaram deliberadamente fora do escopo atual.


## 5. Fonte C — autofocus em câmera e Shape from Focus (Fuhl, Santini, Kasneci)
**Referência:** Wolfgang Fuhl, Thiago Santini, Enkelejda Kasneci. *Fast camera focus estimation for gaze-based focus control*. Springer; texto enviado pelo usuário em `Texto colado(20261102-065842).txt`. Ano/DOI exatos não identificados no texto recebido; não inventar.
**Objetivo real do artigo:** mapear o olhar de um usuário para um ponto da imagem, estimar o plano de profundidade correspondente e alterar o foco eletrônico de uma câmera. NÃO é implementação direta de autofocus contínuo de vídeo com uma lente STM tradicional.
**Setup:** XIMEA mq013mge2 a 60 Hz, imagem processada 640×512 (binning), lente Optotune EL-10-30 com resposta nominal de 2,5 ms, eye tracker e computador. Usam stacks de 20 imagens em diferentes configurações de foco para estimar profundidade; houve também datasets com 191 posições. Reportam ~15 ms de pré-processamento por imagem, ~20 ms para gerar mapa em um núcleo i5, ~332 ms de atualização ideal com 20 imagens a 60 fps, ~450 ms incluindo buffer. **Esses valores não podem ser atribuídos ao DIGIC 4 / EF 50mm STM.**

### 5.1 Pipeline conforme a fonte
1. Capturar imagens ordenadas em diferentes configurações de foco (z = índice do plano adquirido, não distância física direta).
2. Estimar força de foco apenas em regiões informativas: bordas e textura; áreas lisas não são bons indicadores.
3. Aplicar filtro baseado em Canny: primeira derivada gaussiana e magnitude, com supressão não máxima e limiar adaptativo. Rejeitar respostas sem borda válida.
4. Acumular, por pixel, a máxima resposta ao longo dos planos de foco e o z correspondente.
5. Selecionar máximos locais numa vizinhança de 8 conectados; ligar candidatos por triangulação de Delaunay; associar rastros de borda entre planos, inclusive com mudança de escala do campo de visão.
6. Refinar profundidade com ajustes Gaussianos em triplas de respostas, agregação por mediana; corrigir com mediana entre vizinhos e interpolar mapa com coordenadas baricêntricas nos triângulos.
7. Selecionar profundidade no ponto correspondente ao olhar e comandar a lente de foco variável.

### 5.2 Equações úteis e verificáveis do texto
Notação: V(x,y,z) = magnitude de foco filtrada por bordas em localização (x,y) e índice de plano z; M(x,y) = resposta máxima. D(x,y) = índice de plano associado. O texto descreve as equações 1 e 2:
```
M(x,y) = max_z V(x,y,z)
D(x,y) = argmax_z V(x,y,z)       se M(x,y)>0
D(x,y) = 0 / inválido          se M(x,y)=0, conforme convenção do artigo
```
**Observação:** a fórmula extraída do artigo para D está fragmentada na exportação HTML; `argmax` é uma tradução operacional da descrição textual, não transcrição literal verificável da equação visual.

Na etapa de triangulação, para um ponto P dentro de um triângulo ABC, o artigo usa coordenadas baricêntricas:
```
a = area(PBC) / area(ABC)
b = area(PAC) / area(ABC)
c = area(PAB) / area(ABC)
D(P) = a*D(A) + b*D(B) + c*D(C)
```
A interpolação é útil para reconstruir um mapa espacial de profundidade, não é necessária para maximizar score em uma única ROI do SmoothAF.

**Parâmetros reportados pelo artigo:** sigma=2 para derivada gaussiana de Canny; limiar adaptativo configurado de modo que 95% não sejam bordas. São parâmetros do experimento, NÃO valores recomendados automaticamente à T3i.
**Ajuste gaussiano em três pontos:** descrito na seção 5.4, por logaritmo de magnitudes positivas para estimar pico subamostral. A equação 6 ficou corrompida/fragmentada na exportação de texto; NÃO reproduzir como fórmula exata sem consultar original. Candidato futuro para refinar pico local apenas com três amostras confiáveis.

### 5.3 Evidências e limitações
- A fonte enfatiza não estimar foco em regiões uniformes e sem bordas: podem gerar profundidade falsa; seleciona bordas fortes e sinaliza áreas não mensuráveis.
- Os resultados foram comparados com GLVM, Modified Laplacian, Laplacian 3D, wavelets e método de profundidade variacional. A pesquisa não demonstra superioridade universal: em alguns datasets GLVM foi melhor; a fonte reconhece artefatos ao interpolar superfícies sem suporte de medidas.
- O método completo necessita múltiplas imagens em diferentes posições de foco, Delaunay, ajustes e memória para profundidade por pixel; é muito mais complexo que a ROI atual do SmoothAF e pode causar hunting visível se tentarmos reproduzir um stack de foco durante vídeo.
- Mudança de foco altera escala/campo de visão (focus breathing) e faz bordas se deslocarem. Isso afeta comparação pixel a pixel de frames antes/depois de movimentos STM; a fonte usa rastros de borda entre planos para tratar parte desse efeito.
- Velocidade da Optotune (~2,5 ms) não representa latência da EF 50 mm STM; não usar ~450 ms como previsão de desempenho da Canon.

### 5.4 O que aproveitar no SmoothAF (decisões, ainda NÃO implementadas)
**Prioridade alta — benchmark/validação:**
1. Calcular uma estimativa de **densidade de bordas/textura válida** na ROI, combinada com score de foco. Se a cena estiver quase lisa, uniformemente escura ou saturada, reportar `LOW_CONFIDENCE` e impedir movimentos baseados num falso score.
2. Testar scores *edge-gated*: Squared Gradient, Brenner ou Laplaciano acumulados somente em pixels com evidência consistente de borda (sem exigir Canny completo inicialmente). Comparar contra scores sem máscara na mesma amostra.
3. Medir mudança de iluminação e deslocamento/escala da região entre pré/pós movimento para detectar falsas variações no score. Comparar efeitos de subject motion, exposure flicker e focus breathing.
4. Registrar taxa de pixels válidos, saturação, magnitude mediana e ruído, além de score e latência; usar isso como gate de confiança, não para mover lente em modo benchmark.

**Prioridade média — experimentar depois:**
5. Pequena **varredura local de 3 posições** em bancada (sem gravação) com sujeito estático e passos controlados; comparar ajuste parabólico simples e Gaussiano de três pontos. Só integrar controle quando existir medida válida do antes e depois, reversão e retorno seguros.
6. ROI ponderada por estruturas informativas, se a ROI central de 64×64 for pouco texturizada; sem necessidade de mapa 3D completo.

**Adiar / fora do escopo atual:**
- Eye tracking, gaze-to-focus, mapa de profundidade completo, triangulação de Delaunay em tempo real, interpolação de superfície por triângulos e aquisição de 20+ planos para cada atualização de foco. Seriam caros e invasivos para o propósito de AF contínuo de vídeo na T3i.

### 5.5 Plano mínimo de teste derivado do artigo
- Em **Movie Dry Run**, comparar superfície lisa vs objeto com borda forte, pouca luz vs iluminação boa, e objeto em movimento lateral sem mudança de distância. O correto é baixo grau de confiança quando não há evidência de nitidez, não comandos repetidos.
- Em **Movie Active**, só depois de validar compatibilidade/STOP/PANIC, comparar score com máscara de borda e sem máscara em paralelo, primeiro em modo observação. Avaliar tempo de encontrar foco, hunting quando já focado, falhas em regiões pouco texturizadas e estabilidade sob mudança de exposição.
- Câmera filmando vídeo + celular filmando a telemetria: sincronizar por evento visual simples, anotar estado, validade da medição e tempo/contagem de movimentos. Não presumir overlay ML disponível durante gravação.

## 6. Sequência de implementação revista após as três fontes
**(1) Curto prazo:** concluir e validar ACTIVE CONTINUOUS / Movie Dry Run / Movie Active com os mecanismos atuais; não mexer simultaneamente nas métricas do controlador.
**(2) Benchmark paralelo de métricas:** Squared Gradient, Brenner, Tenengrad, Laplaciano 5 pontos, Laplaciano 3×3 da fonte B, contagem de inversões da fonte B; medir custo/ruído/consistência em ROI YUV422.
**(3) Qualidade/confiança:** adicionar indicador de densidade de bordas/textura e pixels saturados ou muito escuros; testar gating e máscaras leves antes de Canny completo.
**(4) Ajuste fino:** estudar ajuste de pico em três pontos e controle anti-hunting após obter curvas de foco reais da EF STM.
**(5) Só após resultados físicos:** escolher algoritmo/fusão para comandar a lente, com rollback claro e STOP/PANIC intactos.

## 7. Perguntas em aberto
- A telemetria/overlay permanece visível ao gravar Movie na T3i com a versão ML instalada?
- Que latência de `lens_focus()` + settle + primeira ROI válida existe em Movie Mode?
- As métricas SAD/Laplaciano atuais são realmente sensíveis a erro de foco, independentemente de movimento do sujeito e mudanças de exposição?
- O usuário pretende selecionar apenas a ROI central ou regiões diferentes (rostos/objetos) numa fase posterior?
- Como garantir representatividade de bordas em ROI 64×64 sem amplificar ruído ou provocar hunting?

## 8. Fonte D — Camera Focus in Computer Vision: A Guide (Timothy M, 26 nov 2024)
**Fonte recebida:** `Texto colado(20261102-070024).txt`. Guia técnico/prático, não um experimento comparativo controlado de autofocus embarcado. Mostra exemplos Python/OpenCV para Variance of Laplacian, Brenner e Tenengrad, com limiares de alerta calibrados à aplicação.

### 8.1 Variance of Laplacian
Para a ROI `I`, calcule o mapa `L = Laplacian(I)` com coeficientes assinados; `muL = (1/N) SUM L[p]` e
```
VAR_LAP = (1/N) SUM_p (L[p] - muL)^2
       = (SUM_p L[p]^2)/N - (SUM_p L[p]/N)^2
```
A segunda identidade permite acumular soma e soma dos quadrados sem alocar toda a imagem intermediária. Usar tipos com intervalo suficiente e não truncar respostas negativas antes de somar. Diferenciar esta métrica de **Energy of Laplacian** `SUM L[p]^2` do primeiro artigo: a variância subtrai o quadrado da média e divide por N; não são identicamente equivalentes em toda ROI, especialmente em bordas/recortes. O exemplo da fonte usa `cv2.Laplacian(gray, cv2.CV_64F)` e `laplacian.var()`. O kernel exato de OpenCV depende dos parâmetros/versão; não afirmar equivalência automática ao Laplaciano do SmoothAF. `focus_threshold=600` é exemplo, NÃO limite universal.

### 8.2 Brenner — variante prática diferente da fórmula clássica
A fonte implementa `dh = max(I[y,x+2]-I[y,x],0)` e `dv=max(I[y+2,x]-I[y,x],0)`, e:
```
BR_VARIANT = (1/N) SUM_{x,y} [max(dh(x,y), dv(x,y))]^2
```
As bordas sem vizinhos válidos recebem zero. **Atenção:** usar `clip(..., 0)` torna o algoritmo sensível ao sinal da borda; não é a formulação clássica simétrica por diferenças ao quadrado, nem o Brenner horizontal com threshold do artigo de 2021. Comparar ambas as variantes no benchmark; a variante simétrica bidirecional (`dx²+dy²` ou `max(dx²,dy²)`, com diferenças assinadas) é proposta adicional, não fórmula da fonte. O código da fonte normaliza imagem para visualização dividindo por `focus_measure.max()` sem tratar máximo zero; no SmoothAF, tratar ROI uniforme como LOW_CONFIDENCE/zero sem divisão por zero. Usar diferença assinada de ao menos 16 bits, acumulador grande.

### 8.3 Tenengrad — variante de magnitude média
A fonte usa Sobel `ksize=5` para `sx,sy`, magnitude euclidiana e média:
```
TEN_MAG = (1/N) SUM_p sqrt(sx[p]^2 + sy[p]^2)
```
**Não confundir** com Tenengrad quadrático do artigo de 2021 `SUM (Gx²+Gy²)`! Custos e escala diferentes (raiz quadrada por pixel vs soma de quadrados). Benchmark deve etiquetar a variante e o tamanho do kernel (3×3 vs 5×5), usando amostras idênticas. No DIGIC 4, priorizar versão quadrática com operações inteiras, se robustez justificar.

### 8.4 Aplicação futura
- Incluir `VAR_LAP` como candidato adicional no benchmark read-only de Movie Dry Run, sem alterar comandos da lente.
- Não copiar os thresholds 500/600 dos exemplos OpenCV: eles são dependentes de ROI, bit depth, kernel, normalização, sensor/ruído.
- Validar FPs em padrões de alto contraste, textura lisa, iluminação variável, ISO alto e movimento lateral. Registrar `score`, tempo por frame, taxa de confiança e estabilidade temporal.
- Um alerta de `Out of Focus` não revela **direção de movimento** nem prova convergência do controlador; separar avaliação da imagem de estratégia de busca.

## 9. Fonte E — Autofocus industrial com fuzzy entropy + busca multiescala
**Fonte recebida:** `Texto colado (2)(6).txt`. Texto de artigo científico de autofocus para medição industrial, comparando avaliação por **entropia fuzzy** e uma estratégia combinada de **busca global coarse-to-fine + ajuste de curva**. O trecho exportado omite as imagens das equações (1) a (10), portanto **não inventar as fórmulas exatas de entropia fuzzy do paper**.

### 9.1 Avaliação por entropia fuzzy — o que o texto afirma
- Normaliza luminância `f(i,j)` para `[0,1]` para reduzir sensibilidade às condições de iluminação.
- Para cada pixel central, `k=f(i,j)`; compara níveis dos vizinhos em uma janela `w × w` com uma **função de pertinência** `mu_k(f)` cujo valor está em `[0,5;1]`, é máximo em `f=k` e diminui simetricamente com `|f-k|`.
- Aplica uma função de entropia fuzzy às pertinências dos vizinhos e combina médias locais num mapa de contraste `m_k(i,j)`, acumulado na ROI de foco para gerar o score final.
- `w=5` na experiência; janela pequena é mais sensível a ruído, janela grande suaviza mas reduz seletividade.
- Artigo relata curva de nitidez estreita e redução de influência de luz/ruído frente a Roberts, Laplaciano, Brenner e Tenengrad nas cenas industriais estudadas; não extrapolar para T3i sem benchmark.
- Fórmulas originais (1) pertinência, (2) entropia, (3) contraste, (4) agregação: **A RECUPERAR de figuras/HTML original, não presentes no texto**. Sem elas, não há implementação fiel garantida.

### 9.2 Estratégia de busca — separar conceito do controlador
1. Amostrar posições de foco em passos relativamente grandes numa faixa limitada (*coarse search*).
2. Localizar a região com melhor score e reduzir faixa e passo (*fine search*).
3. Iterar enquanto a posição estimada variar além da tolerância.
4. Ajustar uma parábola `y(x)=a*x²+b*x+c` aos scores locais da região do pico; se `a<0` e a curva for bem condicionada, o vértice estimado é `x*=-b/(2*a)` (identidade matemática geral, NÃO transcrição literal da equação (7) ausente no texto).
5. Aplicar somente se pico, posição e medições forem válidos; se estimativa ficar fora do intervalo observado ou exigir movimento não permitido, recuar ao melhor ponto medido. Não fazer varredura completa enquanto grava vídeo como comportamento padrão.

**Limitação prática crucial:** o artigo relata ~12,2 segundos por autofocus na plataforma industrial, incluindo operação do motor; não é demonstração de tempo real de vídeo na T3i. Movimento da lente, settle, captura, novos frames e mudanças no sujeito dominam o tempo. Para Movie AF, priorizar microbusca local e manter o foco quando já bom.

### 9.3 Decisões propostas (NÃO implementadas)
- **Alta prioridade:** no benchmark offline/host, comparar curvas de nitidez de `VAR_LAP`, SG, Brenner, Tenengrad e métodos anteriores sob oscilação de exposição e ruído. Adicionar detecção de múltiplos picos e medição do custo por ROI.
- **Média prioridade:** testar uma **parábola de três pontos** em amostras com posições relativas conhecidas para refinamento fino; comparar com ajuste gaussiano mencionado na Fonte C. Ter fallback seguro e nunca assumir eixo de posição STM linear/preciso sem calibrar.
- **Exploratório:** implementar entropia fuzzy com fórmula original verificável; antes, medir custo de janela 5×5, diferenças em `[0,1]`/LUT e comparar robustez com as métricas simples.
- **Adiar:** global sweep de toda a faixa de foco durante gravação, por provocar hunting/desfoque visível e atrasos altos. Considerar busca coarse-to-fine apenas fora de Movie ou para recuperação deliberada de foco muito distante, com autorização específica.
- **Validação:** manter STOP/PANIC, armamento explícito, serialização de `lens_focus`, timeout, verificação de score pós-movimento e gates de Movie; não usar métricas novas diretamente para comandar lente antes dos ensaios read-only.

## 10. Roadmap consolidado após cinco fontes
1. **Base física primeiro:** terminar ACTIVE CONTINUOUS e validar, separadamente, Movie Dry Run e possibilidade segura de Movie Active na 600D. Nenhum artigo prova compatibilidade com Magic Lantern ou o desempenho da EF STM.
2. **Benchmark read-only por frame:** SG clássico; Brenner clássico e variante positiva X/Y; Tenengrad quadrático (3×3) e magnitude média (5×5); Energy Laplacian; Variance Laplacian; contagem de inversões (2025); edge density/valid pixels. Executar mesmo frame/ROI, rotular versão exata, normalizar curvas antes de comparar e medir custo.
3. **Confidence gate:** textura, bordas, pixels muito escuros/saturados, movimento de cena, ruído e validade pós-movimento; publicar razão de não movimento.
4. **Estimar pico com poucas amostras:** testar curva parabólica e gaussiana com três pontos válidos, passo pequeno e fallback ao melhor ponto real, sem alterar o controlador atual durante benchmark.
5. **Entropia fuzzy exploratória:** recuperar fórmulas originais e custo; implementar para comparação somente se trouxer ganho relevante em pouca luz ou mudanças de exposição.
6. **Decisão de controle após evidências:** escolher métrica ou fusão para ACTIVE com mecanismos anti-hunting, tempo de estabilização calibrado à lente e STOP/PANIC intactos.

**Convenção deste caderno:** 'Fonte' = afirmação derivada de material enviado; 'Proposta' = hipótese de engenharia ainda NÃO validada na Canon; 'Fórmula original ausente' = não reconstruir como se fosse transcrição do paper.
