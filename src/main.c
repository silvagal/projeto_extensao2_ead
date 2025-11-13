#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#define MAX_RECORDS 300

static const char *TAG = "APP";

/* ===== Pinos do SD em SPI (VSPI padrão) =====
 * Módulo SD (SPI): VCC, GND, SCK, DO, DI, CS
 *   DO -> MISO
 *   DI -> MOSI
 *   SCK -> CLK
 *   CS -> chip select
 */
#define PIN_NUM_MISO  19   // DO do módulo SD
#define PIN_NUM_MOSI  23   // DI do módulo SD
#define PIN_NUM_CLK   18   // SCK do módulo SD
#define PIN_NUM_CS    5    // CS do módulo SD

/* ===== Dados em memória ===== */
static int   gDay[MAX_RECORDS];
static int   gMonth[MAX_RECORDS];
static int   gYear[MAX_RECORDS];
static float gLitros[MAX_RECORDS];
static float gDuracaoMin[MAX_RECORDS];
static char  gTempoStr[MAX_RECORDS][9];  // "HH:MM:SS" + '\0'
static float gResid[MAX_RECORDS];
static float gAbsResid[MAX_RECORDS];
static int   gIsOutlier[MAX_RECORDS];

static int   gCount       = 0;
static float gSlope       = 0.0f;
static float gIntercept   = 0.0f;
static float gThrAbsResid = 0.0f;

/* ===== Funções auxiliares ===== */

static int read_int_from_stdin(const char *prompt) {
    char buf[32];
    int value = 0;

    printf("%s", prompt);
    fflush(stdout);

    while (1) {
        if (fgets(buf, sizeof(buf), stdin) != NULL) {
            buf[strcspn(buf, "\r\n")] = 0;  // remove \n
            if (strlen(buf) > 0) {
                value = atoi(buf);
                break;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    return value;
}

static void print_alert(int i) {
    char dataBuf[16];
    snprintf(dataBuf, sizeof(dataBuf),
             "%02d/%02d/%04d", gDay[i], gMonth[i], gYear[i]);

    printf("[ALERTA] Dia: %s | Duração: %s | Litros: %.2f L\n",
           dataBuf, gTempoStr[i], gLitros[i]);
}

static void show_outliers_all(void) {
    printf("\nOutliers em TODO o período:\n\n");
    int found = 0;
    for (int i = 0; i < gCount; i++) {
        if (gIsOutlier[i]) {
            print_alert(i);
            found = 1;
        }
    }
    if (!found) {
        printf("Nenhum outlier encontrado nesse critério.\n");
    }
}

static void show_outliers_year(int year) {
    printf("\nOutliers no ano de %d:\n\n", year);
    int found = 0;
    for (int i = 0; i < gCount; i++) {
        if (gIsOutlier[i] && gYear[i] == year) {
            print_alert(i);
            found = 1;
        }
    }
    if (!found) {
        printf("Nenhum outlier encontrado para esse ano.\n");
    }
}

static void show_outliers_month_year(int month, int year) {
    printf("\nOutliers em %02d/%04d:\n\n", month, year);
    int found = 0;
    for (int i = 0; i < gCount; i++) {
        if (gIsOutlier[i] && gYear[i] == year && gMonth[i] == month) {
            print_alert(i);
            found = 1;
        }
    }
    if (!found) {
        printf("Nenhum outlier encontrado para esse mês/ano.\n");
    }
}

static void print_menu(void) {
    printf("\n===== MENU DE OUTLIERS =====\n");
    printf("1 - Ver outliers de TODO o período\n");
    printf("2 - Ver outliers por ANO\n");
    printf("3 - Ver outliers por MES/ANO\n");
    printf("============================\n");
}

/* ===== Inicialização do SD Card via SDSPI ===== */

static esp_err_t init_sdcard(void) {
    esp_err_t ret;
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    ESP_LOGI(TAG, "Montando SD card em %s", mount_point);

    // Config do host SDSPI (usa VSPI / HSPI internamente)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    // Config do barramento SPI
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    // Inicializa o bus SPI no "slot" do host SDSPI
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar bus SPI (%s)", esp_err_to_name(ret));
        return ret;
    }

    // Config do dispositivo SD modo SPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;     // pino CS
    slot_config.host_id = host.slot;      // host/slot usados

    // Config do FATFS
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Monta o SD card
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
                                  &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount falhou (%s)", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

/* ===== Leitura do arquivo + regressão + outliers ===== */

static void load_and_process_file(void) {
    FILE *f = fopen("/sdcard/medicoes.txt", "r");
    if (!f) {
        ESP_LOGE(TAG, "Nao foi possivel abrir /sdcard/medicoes.txt");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    gCount = 0;
    char line[128];

    while (fgets(line, sizeof(line), f) != NULL && gCount < MAX_RECORDS) {
        char dateStr[16], litrosStr[32], tempoStr[16];

        if (sscanf(line, "%15s %31s %15s", dateStr, litrosStr, tempoStr) != 3) {
            continue;
        }

        size_t lenL = strlen(litrosStr);
        if (lenL > 0 && litrosStr[lenL - 1] == 'L') {
            litrosStr[lenL - 1] = '\0';
        }

        int d = 0, m = 0, y = 0;
        if (sscanf(dateStr, "%d/%d/%d", &d, &m, &y) != 3) {
            continue;
        }

        int hh = 0, mm = 0, ss = 0;
        if (sscanf(tempoStr, "%d:%d:%d", &hh, &mm, &ss) != 3) {
            continue;
        }

        float litros = atof(litrosStr);
        long totalSeconds = hh * 3600L + mm * 60L + ss;
        float durMin = totalSeconds / 60.0f;

        if (durMin <= 0.0f) {
            continue;
        }

        gDay[gCount]        = d;
        gMonth[gCount]      = m;
        gYear[gCount]       = y;
        gLitros[gCount]     = litros;
        gDuracaoMin[gCount] = durMin;

        strncpy(gTempoStr[gCount], tempoStr, sizeof(gTempoStr[gCount]) - 1);
        gTempoStr[gCount][sizeof(gTempoStr[gCount]) - 1] = '\0';

        gCount++;
    }

    fclose(f);

    if (gCount == 0) {
        ESP_LOGE(TAG, "Nenhuma medicao valida encontrada.");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    printf("Total de medicoes lidas: %d\n", gCount);

    // Regressão linear
    double sumX = 0.0, sumY = 0.0;
    for (int i = 0; i < gCount; i++) {
        sumX += gDuracaoMin[i];
        sumY += gLitros[i];
    }
    double meanX = sumX / gCount;
    double meanY = sumY / gCount;

    double s_xx = 0.0, s_xy = 0.0;
    for (int i = 0; i < gCount; i++) {
        double dx = gDuracaoMin[i] - meanX;
        double dy = gLitros[i]     - meanY;
        s_xx += dx * dx;
        s_xy += dx * dy;
    }

    if (s_xx == 0.0) {
        ESP_LOGE(TAG, "Nao foi possivel calcular regressao (s_xx == 0).");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    gSlope     = (float)(s_xy / s_xx);
    gIntercept = (float)(meanY - gSlope * meanX);

    for (int i = 0; i < gCount; i++) {
        float y_pred = gSlope * gDuracaoMin[i] + gIntercept;
        gResid[i]    = gLitros[i] - y_pred;
        gAbsResid[i] = fabsf(gResid[i]);
    }

    // Percentil 90 dos |residuos|
    float sortedAbs[MAX_RECORDS];
    for (int i = 0; i < gCount; i++) {
        sortedAbs[i] = gAbsResid[i];
    }

    for (int i = 0; i < gCount - 1; i++) {
        for (int j = i + 1; j < gCount; j++) {
            if (sortedAbs[j] < sortedAbs[i]) {
                float tmp = sortedAbs[i];
                sortedAbs[i] = sortedAbs[j];
                sortedAbs[j] = tmp;
            }
        }
    }

    int idx = (int)(0.9f * (gCount - 1));
    if (idx < 0) idx = 0;
    if (idx >= gCount) idx = gCount - 1;
    gThrAbsResid = sortedAbs[idx];

    int qtdOut = 0;
    for (int i = 0; i < gCount; i++) {
        gIsOutlier[i] = (gAbsResid[i] > gThrAbsResid) ? 1 : 0;
        if (gIsOutlier[i]) qtdOut++;
    }

    printf("\nCoeficientes da reta (litros = a * duracao + b):\n");
    printf("  a = %.3f L/min\n", gSlope);
    printf("  b = %.3f L\n", gIntercept);
    printf("Limiar de outlier (|residuo| > %.2f L)\n", gThrAbsResid);
    printf("Quantidade de outliers detectados: %d\n\n", qtdOut);
}

/* ===== app_main ===== */

void app_main(void) {
    printf("Iniciando sistema de detecao de outliers...\n");

    if (init_sdcard() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar SD card");
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    load_and_process_file();

    while (1) {
        print_menu();
        int opt = read_int_from_stdin("Opcao: ");

        switch (opt) {
            case 1:
                show_outliers_all();
                break;
            case 2: {
                int year = read_int_from_stdin("Digite o ano (ex: 2025): ");
                show_outliers_year(year);
                break;
            }
            case 3: {
                int month = read_int_from_stdin("Digite o mes (1-12): ");
                int year  = read_int_from_stdin("Digite o ano (ex: 2025): ");
                show_outliers_month_year(month, year);
                break;
            }
            default:
                printf("Opcao invalida.\n");
                break;
        }

        printf("\nPressione ENTER para voltar ao menu...");
        fflush(stdout);
        char dummy[8];
        fgets(dummy, sizeof(dummy), stdin);
    }
}