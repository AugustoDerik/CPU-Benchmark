#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <stdint.h>
#include <inttypes.h>

// Quantidade total de iteracoes para cada teste e sua respectiva referencia
constexpr uint32_t TOTAL_ITERATIONS = 400000UL;

// Configuracoes das tarefas do FreeRTOS
constexpr uint32_t WORKER_STACK_SIZE = 4096;
constexpr UBaseType_t WORKER_PRIORITY = 2;
constexpr UBaseType_t COORDINATOR_PRIORITY = 3;
constexpr BaseType_t CORE_0 = 0;
constexpr BaseType_t CORE_1 = 1;

// Variavel global compartilhada para o Caso B
static volatile uint64_t g_sharedCounter = 0;
static SemaphoreHandle_t g_counterMutex = nullptr;

// Estrutura de contexto para sincronizacao e passagem de dados de cada tarefa
struct TaskContext {
    uint32_t startIndex;
    uint32_t endIndex;
    volatile uint64_t localResult;
    SemaphoreHandle_t readySemaphore;
    SemaphoreHandle_t startSemaphore;
    SemaphoreHandle_t doneSemaphore;
    SemaphoreHandle_t sharedMutex;
};

// Estrutura para consolidar os dados de execucao de cada teste
struct BenchmarkMetrics {
    bool success;
    int64_t elapsedUs;
    double elapsedMs;
    uint64_t task0Result;
    uint64_t task1Result;
    uint64_t combinedResult;
};

// Operacao computacional deterministica para o Caso A
static inline uint64_t executeComputationStep(uint32_t index) {
    uint64_t value = static_cast<uint64_t>(index) ^ 0x9E3779B97F4A7C15ULL;
    for (int step = 0; step < 8; ++step) {
        value ^= value >> 30;
        value *= 0xBF58476D1CE4E5B9ULL;
        value ^= value >> 27;
        value *= 0x94D049BB133111EBULL;
        value ^= value >> 31;
    }
    // Impede que o compilador elimine o calculo por otimizacao
    asm volatile("" : "+r"(value));
    return value;
}

// Tarefa para processamento de dados independentes (Referencia A e Caso A)
static void independentWorkerTask(void *pvParameters) {
    TaskContext *ctx = static_cast<TaskContext *>(pvParameters);

    // Sinaliza que a tarefa foi criada e esta pronta para iniciar
    xSemaphoreGive(ctx->readySemaphore);

    // Aguarda autorizacao sincronizada para iniciar o processamento
    xSemaphoreTake(ctx->startSemaphore, portMAX_DELAY);

    uint64_t accumulator = 0;
    for (uint32_t i = ctx->startIndex; i < ctx->endIndex; ++i) {
        accumulator += executeComputationStep(i);
    }

    ctx->localResult = accumulator;

    // Sinaliza conclusao do trabalho
    xSemaphoreGive(ctx->doneSemaphore);
    vTaskDelete(nullptr);
}

// Tarefa de referencia sequencial para atualizacao do contador sem mutex (Referencia B)
static void sequentialCounterTask(void *pvParameters) {
    TaskContext *ctx = static_cast<TaskContext *>(pvParameters);

    xSemaphoreGive(ctx->readySemaphore);
    xSemaphoreTake(ctx->startSemaphore, portMAX_DELAY);

    uint64_t localUpdates = 0;
    uint32_t iterations = ctx->endIndex - ctx->startIndex;
    for (uint32_t i = 0; i < iterations; ++i) {
        g_sharedCounter = g_sharedCounter + 1;
        localUpdates++;
    }

    ctx->localResult = localUpdates;

    xSemaphoreGive(ctx->doneSemaphore);
    vTaskDelete(nullptr);
}

// Tarefa paralela para atualizacao da variavel compartilhada protegida por mutex (Caso B)
static void mutexCounterTask(void *pvParameters) {
    TaskContext *ctx = static_cast<TaskContext *>(pvParameters);

    xSemaphoreGive(ctx->readySemaphore);
    xSemaphoreTake(ctx->startSemaphore, portMAX_DELAY);

    uint64_t localUpdates = 0;
    uint32_t iterations = ctx->endIndex - ctx->startIndex;
    for (uint32_t i = 0; i < iterations; ++i) {
        if (xSemaphoreTake(ctx->sharedMutex, portMAX_DELAY) == pdTRUE) {
            g_sharedCounter = g_sharedCounter + 1;
            xSemaphoreGive(ctx->sharedMutex);
            localUpdates++;
        }
    }

    ctx->localResult = localUpdates;

    xSemaphoreGive(ctx->doneSemaphore);
    vTaskDelete(nullptr);
}

// Inicializa os semaforos de controle de um contexto de tarefa
static bool initTaskSync(TaskContext &ctx) {
    ctx.readySemaphore = xSemaphoreCreateBinary();
    ctx.startSemaphore = xSemaphoreCreateBinary();
    ctx.doneSemaphore = xSemaphoreCreateBinary();

    return (ctx.readySemaphore != nullptr) &&
           (ctx.startSemaphore != nullptr) &&
           (ctx.doneSemaphore != nullptr);
}

// Libera os semaforos de controle de um contexto de tarefa
static void cleanupTaskSync(TaskContext &ctx) {
    if (ctx.readySemaphore != nullptr) {
        vSemaphoreDelete(ctx.readySemaphore);
        ctx.readySemaphore = nullptr;
    }
    if (ctx.startSemaphore != nullptr) {
        vSemaphoreDelete(ctx.startSemaphore);
        ctx.startSemaphore = nullptr;
    }
    if (ctx.doneSemaphore != nullptr) {
        vSemaphoreDelete(ctx.doneSemaphore);
        ctx.doneSemaphore = nullptr;
    }
}

// Executa um teste com uma unica tarefa (Referencia)
static BenchmarkMetrics runSingleTaskTest(TaskFunction_t taskFunction, const char *taskName, BaseType_t coreId) {
    BenchmarkMetrics metrics = {false, 0, 0.0, 0, 0, 0};
    TaskContext ctx = {0, TOTAL_ITERATIONS, 0, nullptr, nullptr, nullptr, nullptr};

    if (!initTaskSync(ctx)) {
        Serial.println("[ERRO] Falha ao alocar semaforos de sincronizacao para o teste sequencial.");
        cleanupTaskSync(ctx);
        return metrics;
    }

    TaskHandle_t taskHandle = nullptr;
    BaseType_t created = xTaskCreatePinnedToCore(
        taskFunction,
        taskName,
        WORKER_STACK_SIZE,
        &ctx,
        WORKER_PRIORITY,
        &taskHandle,
        coreId
    );

    if (created != pdPASS || taskHandle == nullptr) {
        Serial.println("[ERRO] Falha ao criar a tarefa de referencia.");
        cleanupTaskSync(ctx);
        return metrics;
    }

    // Aguarda a tarefa estar pronta antes de iniciar a medicao de tempo
    xSemaphoreTake(ctx.readySemaphore, portMAX_DELAY);

    int64_t startTimeUs = esp_timer_get_time();
    xSemaphoreGive(ctx.startSemaphore);
    xSemaphoreTake(ctx.doneSemaphore, portMAX_DELAY);
    int64_t endTimeUs = esp_timer_get_time();

    // Aguarda brevemente o encerramento completo da tarefa antes de liberar recursos
    vTaskDelay(pdMS_TO_TICKS(10));
    cleanupTaskSync(ctx);

    metrics.success = true;
    metrics.elapsedUs = endTimeUs - startTimeUs;
    metrics.elapsedMs = static_cast<double>(metrics.elapsedUs) / 1000.0;
    metrics.task0Result = ctx.localResult;
    metrics.task1Result = 0;
    metrics.combinedResult = ctx.localResult;
    return metrics;
}

// Executa um teste com duas tarefas fixadas em nucleos distintos (Caso A e Caso B)
static BenchmarkMetrics runDualTaskTest(TaskFunction_t taskFunction, SemaphoreHandle_t sharedMutex) {
    BenchmarkMetrics metrics = {false, 0, 0.0, 0, 0, 0};

    // Divisao equilibrada das iteracoes considerando eventual resto da divisao
    uint32_t baseIterations = TOTAL_ITERATIONS / 2;
    uint32_t remainder = TOTAL_ITERATIONS % 2;
    uint32_t splitIndex = baseIterations + remainder;

    TaskContext ctxCore0 = {0, splitIndex, 0, nullptr, nullptr, nullptr, sharedMutex};
    TaskContext ctxCore1 = {splitIndex, TOTAL_ITERATIONS, 0, nullptr, nullptr, nullptr, sharedMutex};

    if (!initTaskSync(ctxCore0) || !initTaskSync(ctxCore1)) {
        Serial.println("[ERRO] Falha ao alocar semaforos de sincronizacao para o teste paralelo.");
        cleanupTaskSync(ctxCore0);
        cleanupTaskSync(ctxCore1);
        return metrics;
    }

    TaskHandle_t handleCore0 = nullptr;
    TaskHandle_t handleCore1 = nullptr;

    BaseType_t created0 = xTaskCreatePinnedToCore(
        taskFunction,
        "WorkerCore0",
        WORKER_STACK_SIZE,
        &ctxCore0,
        WORKER_PRIORITY,
        &handleCore0,
        CORE_0
    );

    if (created0 != pdPASS || handleCore0 == nullptr) {
        Serial.println("[ERRO] Falha ao criar a Tarefa 1 no nucleo 0.");
        cleanupTaskSync(ctxCore0);
        cleanupTaskSync(ctxCore1);
        return metrics;
    }

    BaseType_t created1 = xTaskCreatePinnedToCore(
        taskFunction,
        "WorkerCore1",
        WORKER_STACK_SIZE,
        &ctxCore1,
        WORKER_PRIORITY,
        &handleCore1,
        CORE_1
    );

    if (created1 != pdPASS || handleCore1 == nullptr) {
        Serial.println("[ERRO] Falha ao criar a Tarefa 2 no nucleo 1.");
        vTaskDelete(handleCore0);
        cleanupTaskSync(ctxCore0);
        cleanupTaskSync(ctxCore1);
        return metrics;
    }

    // Aguarda ambas as tarefas sinalizarem prontidao antes de iniciar a medicao
    xSemaphoreTake(ctxCore0.readySemaphore, portMAX_DELAY);
    xSemaphoreTake(ctxCore1.readySemaphore, portMAX_DELAY);

    int64_t startTimeUs = esp_timer_get_time();

    // Dispara o inicio sincronizado das duas tarefas
    xSemaphoreGive(ctxCore0.startSemaphore);
    xSemaphoreGive(ctxCore1.startSemaphore);

    // Aguarda a conclusao de ambas as tarefas
    xSemaphoreTake(ctxCore0.doneSemaphore, portMAX_DELAY);
    xSemaphoreTake(ctxCore1.doneSemaphore, portMAX_DELAY);

    int64_t endTimeUs = esp_timer_get_time();

    // Garante que ambas as tarefas finalizaram vTaskDelete antes de remover os semaforos
    vTaskDelay(pdMS_TO_TICKS(10));
    cleanupTaskSync(ctxCore0);
    cleanupTaskSync(ctxCore1);

    metrics.success = true;
    metrics.elapsedUs = endTimeUs - startTimeUs;
    metrics.elapsedMs = static_cast<double>(metrics.elapsedUs) / 1000.0;
    metrics.task0Result = ctxCore0.localResult;
    metrics.task1Result = ctxCore1.localResult;
    metrics.combinedResult = ctxCore0.localResult + ctxCore1.localResult;
    return metrics;
}

// Calcula o speedup evitando divisao por zero
static double calculateSpeedup(int64_t referenceTimeUs, int64_t parallelTimeUs) {
    if (parallelTimeUs <= 0) {
        return 0.0;
    }
    return static_cast<double>(referenceTimeUs) / static_cast<double>(parallelTimeUs);
}

static void printSeparator() {
    Serial.println("------------------------------------------------------------------------");
}

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(1500));

    // =========================================================================
    // CONFIGURACAO DO WATCHDOG TIMER (WDT)
    // =========================================================================
    // Aumenta o timeout do WDT para 30 segundos para evitar falhas durante
    // os testes de estresse com lockings prolongados (Caso B).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 30000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = false
    };
    esp_task_wdt_reconfigure(&wdt_config);
#else
    esp_task_wdt_init(30, false);
#endif
    // =========================================================================

    // Eleva a prioridade da tarefa coordenadora para garantir sincronismo preciso no disparo
    vTaskPrioritySet(nullptr, COORDINATOR_PRIORITY);

    uint32_t task1Iterations = (TOTAL_ITERATIONS / 2) + (TOTAL_ITERATIONS % 2);
    uint32_t task2Iterations = TOTAL_ITERATIONS / 2;

    Serial.println();
    printSeparator();
    Serial.println("BENCHMARK FREERTOS ESP32-S3: EXECUCAO SEQUENCIAL VS. PARALELA");
    printSeparator();
    Serial.printf("Quantidade total de iteracoes por teste : %" PRIu32 "\n", TOTAL_ITERATIONS);
    Serial.printf("Divisao nos testes paralelos            : Tarefa 1 (Core 0) = %" PRIu32 " | Tarefa 2 (Core 1) = %" PRIu32 "\n",
                  task1Iterations, task2Iterations);
    printSeparator();

    // =========================================================================
    // 1. CASO A: DADOS INDEPENDENTES (REFERENCIA SEQUENCIAL E PARALELO)
    // =========================================================================
    Serial.println("\n[CASO A] PROCESSAMENTO COM DADOS INDEPENDENTES");
    printSeparator();

    BenchmarkMetrics refA = runSingleTaskTest(independentWorkerTask, "RefTaskA", CORE_0);
    vTaskDelay(pdMS_TO_TICKS(50));

    BenchmarkMetrics parA = runDualTaskTest(independentWorkerTask, nullptr);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (refA.success && parA.success) {
        double speedupA = calculateSpeedup(refA.elapsedUs, parA.elapsedUs);
        int64_t diffUsA = refA.elapsedUs - parA.elapsedUs;

        Serial.println("1. Teste de Referencia A (1 Tarefa - Sequencial no Core 0):");
        Serial.printf("   - Iteracoes totais       : %" PRIu32 "\n", TOTAL_ITERATIONS);
        Serial.printf("   - Tempo de execucao      : %" PRId64 " us (%.3f ms)\n", refA.elapsedUs, refA.elapsedMs);
        Serial.printf("   - Resultado acumulado    : %" PRIu64 "\n\n", refA.combinedResult);

        Serial.println("2. Teste Paralelo — Caso A (2 Tarefas - Core 0 e Core 1):");
        Serial.printf("   - Iteracoes totais       : %" PRIu32 "\n", TOTAL_ITERATIONS);
        Serial.printf("   - Tempo de execucao      : %" PRId64 " us (%.3f ms)\n", parA.elapsedUs, parA.elapsedMs);
        Serial.printf("   - Resultado Tarefa 1     : %" PRIu64 "\n", parA.task0Result);
        Serial.printf("   - Resultado Tarefa 2     : %" PRIu64 "\n", parA.task1Result);
        Serial.printf("   - Resultado combinado    : %" PRIu64 "\n", parA.combinedResult);
        Serial.printf("   - Consistencia dos dados : %s\n\n",
                      (refA.combinedResult == parA.combinedResult) ? "VALIDO (Resultados identicos)" : "ERRO (Divergencia)");

        Serial.println("3. Comparacao e Desempenho (Caso A):");
        Serial.printf("   - Tempo sequencial       : %" PRId64 " us (%.3f ms)\n", refA.elapsedUs, refA.elapsedMs);
        Serial.printf("   - Tempo paralelo         : %" PRId64 " us (%.3f ms)\n", parA.elapsedUs, parA.elapsedMs);
        Serial.printf("   - Diferenca (Seq - Par)  : %" PRId64 " us (%.3f ms)\n", diffUsA, static_cast<double>(diffUsA) / 1000.0);
        Serial.printf("   - Speedup (Ref / Par)    : %.4fx\n", speedupA);
    } else {
        Serial.println("[ERRO] O benchmark do Caso A nao pode ser concluido.");
    }

    printSeparator();

    // =========================================================================
    // 2. CASO B: VARIAVEL COMPARTILHADA COM MUTEX (REFERENCIA E PARALELO)
    // =========================================================================
    Serial.println("\n[CASO B] PROCESSAMENTO COM VARIAVEL COMPARTILHADA PROTEGIDA POR MUTEX");
    printSeparator();

    g_counterMutex = xSemaphoreCreateMutex();
    if (g_counterMutex == nullptr) {
        Serial.println("[ERRO] Falha ao criar o mutex do FreeRTOS para o Caso B.");
        return;
    }

    g_sharedCounter = 0;
    BenchmarkMetrics refB = runSingleTaskTest(sequentialCounterTask, "RefTaskB", CORE_0);
    uint64_t finalCounterRefB = g_sharedCounter;
    vTaskDelay(pdMS_TO_TICKS(50));

    g_sharedCounter = 0;
    BenchmarkMetrics parB = runDualTaskTest(mutexCounterTask, g_counterMutex);
    uint64_t finalCounterParB = g_sharedCounter;
    vTaskDelay(pdMS_TO_TICKS(50));

    // Libera o mutex somente apos a conclusao de todas as tarefas
    vSemaphoreDelete(g_counterMutex);
    g_counterMutex = nullptr;

    if (refB.success && parB.success) {
        double speedupB = calculateSpeedup(refB.elapsedUs, parB.elapsedUs);
        int64_t diffUsB = refB.elapsedUs - parB.elapsedUs;

        Serial.println("1. Teste de Referencia B (1 Tarefa - Sem Mutex no Core 0):");
        Serial.printf("   - Iteracoes totais       : %" PRIu32 "\n", TOTAL_ITERATIONS);
        Serial.printf("   - Tempo de execucao      : %" PRId64 " us (%.3f ms)\n", refB.elapsedUs, refB.elapsedMs);
        Serial.printf("   - Atualizacoes da tarefa : %" PRIu64 "\n", refB.task0Result);
        Serial.printf("   - Valor final contador   : %" PRIu64 " (Esperado: %" PRIu32 ")\n\n",
                      finalCounterRefB, TOTAL_ITERATIONS);

        Serial.println("2. Teste Paralelo — Caso B (2 Tarefas - Com Mutex no Core 0 e Core 1):");
        Serial.printf("   - Iteracoes totais       : %" PRIu32 "\n", TOTAL_ITERATIONS);
        Serial.printf("   - Tempo de execucao      : %" PRId64 " us (%.3f ms)\n", parB.elapsedUs, parB.elapsedMs);
        Serial.printf("   - Atualizacoes Tarefa 1  : %" PRIu64 "\n", parB.task0Result);
        Serial.printf("   - Atualizacoes Tarefa 2  : %" PRIu64 "\n", parB.task1Result);
        Serial.printf("   - Soma das atualizacoes  : %" PRIu64 "\n", parB.combinedResult);
        Serial.printf("   - Valor final contador   : %" PRIu64 " (Esperado: %" PRIu32 ")\n",
                      finalCounterParB, TOTAL_ITERATIONS);
        Serial.printf("   - Integridade do contador: %s\n\n",
                      (finalCounterParB == TOTAL_ITERATIONS) ? "VALIDO (Sem condicoes de corrida)" : "ERRO (Valor incorreto)");

        Serial.println("3. Comparacao e Desempenho (Caso B):");
        Serial.printf("   - Tempo sequencial       : %" PRId64 " us (%.3f ms)\n", refB.elapsedUs, refB.elapsedMs);
        Serial.printf("   - Tempo paralelo         : %" PRId64 " us (%.3f ms)\n", parB.elapsedUs, parB.elapsedMs);
        Serial.printf("   - Diferenca (Seq - Par)  : %" PRId64 " us (%.3f ms)\n", diffUsB, static_cast<double>(diffUsB) / 1000.0);
        Serial.printf("   - Speedup (Ref / Par)    : %.4fx\n", speedupB);
    } else {
        Serial.println("[ERRO] O benchmark do Caso B nao pode ser concluido.");
    }

    printSeparator();

    // =========================================================================
    // 3. ANALISE TECNICA DOS RESULTADOS MEDIDOS
    // =========================================================================
    Serial.println("\nANALISE DE DESEMPENHO E SINCRONIZACAO:");
    if (refA.success && parA.success) {
        if (parA.elapsedUs < refA.elapsedUs) {
            Serial.println("- Caso A (Dados Independentes): O tempo paralelo foi menor que o sequencial.");
            Serial.println("  Como cada nucleo operou sobre variaveis locais sem disputas de memoria ou");
            Serial.println("  bloqueios, a divisao do trabalho permitiu execucao simultanea real nos dois");
            Serial.println("  nucleos do ESP32-S3, reduzindo o tempo total para perto da metade.");
        } else {
            Serial.println("- Caso A (Dados Independentes): O tempo paralelo nao superou o sequencial.");
            Serial.println("  Para cargas muito curtas, o custo de criacao, escalonamento e sincronizacao");
            Serial.println("  das tarefas pode superar o ganho da execucao distribuida.");
        }
    }

    if (refB.success && parB.success) {
        if (parB.elapsedUs > refB.elapsedUs) {
            Serial.println("- Caso B (Variavel Compartilhada): O tempo paralelo com mutex foi maior que");
            Serial.println("  o tempo sequencial sem mutex (speedup inferior a 1.0x). Embora o mutex");
            Serial.println("  garanta a corretude do valor final do contador, a secao critica serializa");
            Serial.println("  o acesso entre os nucleos 0 e 1. Além de impedir o paralelismo efetivo na");
            Serial.println("  atualizacao, cada iteracao incorre no custo das chamadas xSemaphoreTake()");
            Serial.println("  e xSemaphoreGive(), contencao de spinlock interno do FreeRTOS e trocas de");
            Serial.println("  contexto quando a tarefa concorrente encontra o recurso bloqueado.");
        } else {
            Serial.println("- Caso B (Variavel Compartilhada): O uso do mutex garantiu a integridade");
            Serial.println("  das atualizacoes concorrentes entre os dois nucleos.");
        }
    }
    printSeparator();
    Serial.println("Benchmark finalizado.");
}

void loop() {
    // O benchmark e executado apenas uma vez na inicializacao (setup).
    vTaskDelay(pdMS_TO_TICKS(10000));
}