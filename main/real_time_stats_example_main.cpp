/* FreeRTOS Real Time Stats Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "NMEA_msg.h"
#include "esp_log.h"
#include <N2kMsg.h>
#include <NMEA2000_esp32-c6.h> 
#include <NMEA2000_esp32-c6.h>
#include <NMEA2000.h>
#include <N2kMessages.h>
#include "esp_err.h"

#include "driver/gpio.h"
#include "bi-inc/attr_container.h"

#include "wasm_export.h"
#include "bh_read_file.h"
#include "bh_getopt.h"
#include "bh_platform.h"

#include <queue>
#include <string>
#include <iomanip>


//WebAssembley App
#include "nmea_attack.h" 

#define NATIVE_STACK_SIZE               (32*1024)
#define NATIVE_HEAP_SIZE                (32*1024)
#define PTHREAD_STACK_SIZE              4096
#define MAX_DATA_LENGTH_BTYES           223
#define BUFFER_SIZE                     (10 + 223*2) //10 bytes for id, 223*2 bytes for data
#define MY_ESP_LOG_LEVEL                  ESP_LOG_INFO // the log level for this file

#define SPIN_ITER           500000  //Actual CPU cycles used will depend on compiler optimization
#define SPIN_TASK_PRIO      tskIDLE_PRIORITY //2
#define STATS_TASK_PRIO     tskIDLE_PRIORITY //3
#define STATS_TICKS         pdMS_TO_TICKS(1000)
#define ARRAY_SIZE_OFFSET   5   //Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE

// Tag for ESP logging
static const char* TAG_TWAI_TX = "TWAI_SEND";
static const char* TAG_TWAI_RX = "TWAI_RECEIVE";
static const char* TAG_WASM = "WASM";
static const char* TAG_STATUS = "STATUS";

static TaskHandle_t N2K_task_handle = nullptr;
static TaskHandle_t N2K_spin_handle = nullptr;

/**
 * @brief Creates a NMEA2000 Object
 * 
 * NMEA2000(TX_PIN, RX_PIN)
*/
tNMEA2000_esp32c6 NMEA2000(GPIO_NUM_4, GPIO_NUM_5);

// Task Handles
static TaskHandle_t N2K_task_handle = NULL;
static TaskHandle_t N2K_receive_task_handle = NULL;

static unsigned long N2kMsgSentCount=0;
static unsigned long N2kMsgFailCount=0;

/**
 * @brief   Function to print the CPU usage of tasks over a given duration.
 *
 * This function will measure and print the CPU usage of tasks over a specified
 * number of ticks (i.e. real time stats). This is implemented by simply calling
 * uxTaskGetSystemState() twice separated by a delay, then calculating the
 * differences of task run times before and after the delay.
 *
 * @note    If any tasks are added or removed during the delay, the stats of
 *          those tasks will not be printed.
 * @note    This function should be called from a high priority task to minimize
 *          inaccuracies with delays.
 * @note    When running in dual core mode, each core will correspond to 50% of
 *          the run time.
 *
 * @param   xTicksToWait    Period of stats measurement
 *
 * @return
 *  - ESP_OK                Success
 *  - ESP_ERR_NO_MEM        Insufficient memory to allocated internal arrays
 *  - ESP_ERR_INVALID_SIZE  Insufficient array size for uxTaskGetSystemState. Trying increasing ARRAY_SIZE_OFFSET
 *  - ESP_ERR_INVALID_STATE Delay duration too short
 */
static esp_err_t print_real_time_stats(TickType_t xTicksToWait)
{
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    uint32_t start_run_time, end_run_time;
    esp_err_t ret;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = reinterpret_cast<TaskStatus_t*>(std::malloc(sizeof(TaskStatus_t) * start_array_size));
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        free(start_array);
        free(end_array);
        return ret;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        free(start_array);
        free(end_array);
        return ret;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = reinterpret_cast<TaskStatus_t*>(std::malloc(sizeof(TaskStatus_t) * end_array_size));
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        free(start_array);
        free(end_array);
        return ret;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        free(start_array);
        free(end_array);
        return ret;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        free(start_array);
        free(end_array);
        return ret;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * portNUM_PROCESSORS);
            printf("| %s | %"PRIu32" | %"PRIu32"%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;
    free(start_array);
    free(end_array);
    return ret;

}

static void spin_task(void *arg)
{
    while (1) {
        //Consume CPU cycles
        for (int i = 0; i < SPIN_ITER; i++) {
            __asm__ __volatile__("NOP");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void stats_task(void *arg)
{
    //Print real time stats periodically
    while (1) {
        printf("\n\nGetting real time stats over %"PRIu32" ticks\n", STATS_TICKS);
        if (print_real_time_stats(STATS_TICKS) == ESP_OK) {
            printf("Real time stats obtained\n");
        } else {
            printf("Error getting real time stats\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" int app_main(void)
{
    /* Status Task*/
    esp_err_t result = ESP_OK;
    printf( "create task");
    xTaskCreate(
        &stats_task,            // Pointer to the task entry function.
        "stats_task",           // A descriptive name for the task for debugging.
        4096,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        STATS_TASK_PRIO, // priority at which the task should run
        &N2K_task_handle      // Optional pass back task handle
    );
    if (N2K_task_handle == NULL)
    {
        printf("Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
    /* Spin Task*/
    printf( "create task");
    xTaskCreate(
        &spin_task,            // Pointer to the task entry function.
        "spin1",           // A descriptive name for the task for debugging.
        1024,                 // size of the task stack in bytes.
        NULL,                 // Optional pointer to pvParameters
        SPIN_TASK_PRIO, // priority at which the task should run
        &N2K_spin_handle      // Optional pass back task handle
    );
    if (N2K_spin_handle == NULL)
    {
        printf("Unable to create task.");
        result = ESP_ERR_NO_MEM;
        goto err_out;
    }
err_out:
    if (result != ESP_OK)
    {
        if (N2K_task_handle != NULL)
        {
            vTaskDelete(N2K_task_handle);
            N2K_task_handle = NULL;
        }
    }
    return 0;
}
