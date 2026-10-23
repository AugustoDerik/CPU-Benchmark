# Benchmark de Desempenho FreeRTOS no ESP32-S3: Execução Sequencial vs. Paralela

## Visão Geral

Este repositório contém um projeto desenvolvido em C++ utilizando o framework Arduino e a API nativa do FreeRTOS para o microcontrolador ESP32-S3 (arquitetura *dual-core*). O objetivo principal é analisar o desempenho de tarefas executadas sequencialmente em um núcleo em comparação com a execução paralela em dois núcleos.

O sistema avalia duas situações de carga de trabalho para determinar a eficiência do paralelismo e o impacto do custo computacional (*overhead*) introduzido por mecanismos de sincronização.

## Casos de Teste (Benchmark)

O sistema executa uma quantidade definida de iterações (400.000, por padrão) e mede o tempo de execução utilizando o temporizador de alta resolução `esp_timer_get_time()`.

*   **Caso A: Processamento de Dados Independentes**
    *   **Sequencial (Referência):** Uma única tarefa executa toda a carga de cálculos matemáticos alocada no Core 0.
    *   **Paralelo:** A carga de trabalho é dividida igualmente entre duas tarefas (Core 0 e Core 1). Cada tarefa opera sobre seus próprios dados locais, sem disputa de recursos.
    *   **Objetivo:** Demonstrar o aumento de desempenho (*speedup*) em processamento paralelo autônomo.

*   **Caso B: Acesso a Variável Compartilhada com Mutex**
    *   **Sequencial (Referência):** Uma única tarefa atualiza um contador global sem a necessidade de mecanismos de bloqueio.
    *   **Paralelo:** Duas tarefas alocadas em núcleos distintos atualizam o mesmo contador global. Utiliza-se um *mutex* (`xSemaphoreCreateMutex()`) para criar uma seção crítica, evitando condições de corrida.
    *   **Objetivo:** Avaliar o impacto da contenção do *mutex* e a serialização do acesso à memória no tempo total de execução.

## Requisitos de Hardware e Software

*   **Hardware:** Placa de desenvolvimento baseada no ESP32-S3 (ex: DevKitC-1).
*   **Software:**
    *   [Visual Studio Code](https://code.visualstudio.com/).
    *   Extensão [PlatformIO IDE](https://platformio.org/).
*   **Framework:** Arduino.

## Instruções de Configuração e Uso

1.  Crie um novo projeto no PlatformIO selecionando a sua placa (ex: `Espressif ESP32-S3-DevKitC-1`) e o framework `Arduino`.
2.  Copie o código-fonte fornecido e substitua integralmente o conteúdo do arquivo `src/main.cpp`.
3.  Conecte a placa ESP32-S3 ao computador via USB.
4.  No PlatformIO, clique no botão **Upload** para compilar e transferir o *firmware* para o microcontrolador.
5.  Abra o **Serial Monitor** do PlatformIO. Certifique-se de que a taxa de transmissão (*Baud Rate*) esteja configurada para `115200` bps no arquivo `platformio.ini` ou na interface da extensão.
    *   *Exemplo de configuração no platformio.ini:* `monitor_speed = 115200`
6.  Pressione o botão de *Reset* físico na placa (se necessário) e acompanhe a execução do benchmark no monitor serial.

## Análise de Resultados Esperada

Ao final da inicialização do dispositivo, o monitor serial exibirá os tempos de execução em microssegundos e calculará a métrica de *speedup* (Tempo de Referência / Tempo Paralelo).

*   **Para o Caso A:** É esperado um *speedup* próximo a 2.0x (ou ligeiramente inferior devido aos custos de criação e sincronização das tarefas). O paralelismo é eficiente por não haver dependência de dados entre os núcleos.
*   **Para o Caso B:** É esperado que a execução paralela seja consideravelmente mais lenta que a execução sequencial de referência (*speedup* inferior a 1.0x). Isso ocorre porque o *mutex* serializa a operação de incremento, forçando os núcleos a aguardarem a liberação do recurso. Além da perda do paralelismo prático na seção crítica, o sistema incorre no custo computacional de gerenciar o *lock* e realizar mudanças de contexto repetidas vezes. A corretude dos dados, no entanto, é garantida (o contador atingirá exatamente o valor esperado).
