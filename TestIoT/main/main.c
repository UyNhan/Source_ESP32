#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_check.h"
#include "nvs.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "sdkconfig.h"
#include "math.h"
#include "button.c"
#include "esp_sntp.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "ds1307.h"
#include "file_server.c"
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/param.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#include <time.h>
#include <sys/time.h>
#include "esp_event_loop.h"
#include "esp_task_wdt.h"
#include "esp_err.h"
#include "esp_sleep.h"


#define TAG "MQTT_DAQ"
#define STORAGE_NAMESPACE "esp32_storage" // save variable to nvs storage

#define TIMER_DIVIDER         (16)  //  Hardware timer clock divider
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // convert counter value to seconds

// Define pin to read InjectCycle, injectTime, badProduct
#define GPIO_INPUT_CYCLE_1   34
#define GPIO_INPUT_CYCLE_2   39
#define GPIO_INPUT_BAD_1     36
#define GPIO_INPUT_BAD_2     35
#define GPIO_INPUT_BAD_3     13
#define GPIO_INPUT_BAD_4     15
#define GPIO_INPUT_SETUP_1    5
#define GPIO_INPUT_SETUP_2    4
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_CYCLE_1) | (1ULL<<GPIO_INPUT_CYCLE_2)| (1ULL<<GPIO_INPUT_BAD_1)| (1ULL<<GPIO_INPUT_BAD_2)| (1ULL<<GPIO_INPUT_BAD_3)| (1ULL<<GPIO_INPUT_BAD_4)| (1ULL<<GPIO_INPUT_SETUP_1)| (1ULL<<GPIO_INPUT_SETUP_2))

#define ESP_INTR_FLAG_DEFAULT 0

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   14

#define SPI_DMA_CHAN    1
#define MOUNT_POINT "/sdcard"

#define SSID CONFIG_WIFI_SSID
#define PASSWORD CONFIG_WIFI_PASSWORD

typedef struct {
    int timer_group;
    int timer_idx;
    int alarm_interval;
    bool auto_reload;
} timer_info_t;

typedef struct 
{
  char *timestamp;    
  int command; 
  bool is_configured;
} cfg_info_t; 

typedef struct 
{
  int counter_shot;
  int NGFront;
  int NGBack;
  char *timestamp;
  double cycle_time;
  double injectionTime;
  int operationTime;
  bool mode_working; 
  int status;  
}cycle_info_t;

typedef struct 
{
  char message_text[500];
  char message_mqtt[500];
  char mqtt_injectionTime[500];
  char mqtt_injectionCycle[500];
  char mqtt_counterShot[500];
  char mqtt_shiftNumber[500];
  char mqtt_operationTime[500];
  char mqtt_badProductFront[500];
  char mqtt_badProductBack[500];
} message_info_t;

enum feedback {SychTime, SDcardfail, RTCfail};
enum da_mess {ChangeMold, ChangeMoldDone, Reboot};
enum status {None,Run,Alarm,Idle,Setup,Off};
static esp_mqtt_client_handle_t client = NULL;
static xQueueHandle mqtt_mess_events;
static xQueueHandle gpio_evt_queue = NULL;
SemaphoreHandle_t i2cSemaphore = NULL;
TaskHandle_t taskHandle;
TaskHandle_t initiate_taskHandle;
TimerHandle_t soft_timer_handle_1;
TimerHandle_t soft_timer_handle_2;
TimerHandle_t soft_timer_handle_3;
TimerHandle_t soft_timer_handle_4;
TimerHandle_t soft_timer_handle_5;
TimerHandle_t soft_timer_handle_6;
TimerHandle_t soft_timer_handle_7;
TimerHandle_t soft_timer_handle_8;
i2c_dev_t rtc_i2c;
cycle_info_t cycle_info_1;
cycle_info_t cycle_info_2;
cfg_info_t cfg_info_1;
cfg_info_t cfg_info_2;
message_info_t message_info_1;
message_info_t message_info_2;

struct tm local_time;
struct tm inject_time_1;
struct tm inject_time_2;
struct tm alarm_time;
struct tm idle_time;
struct tm lwt_time;

const uint32_t WIFI_CONNEECTED = BIT1;
const uint32_t MQTT_CONNECTED = BIT2;
const uint32_t MQTT_PUBLISHED = BIT3;
const uint32_t SYSTEM_READY   = BIT4;
const uint32_t INITIATE_SETUP = BIT5;
const uint32_t SYNC_TIME_DONE = BIT6;
const uint32_t SYNC_TIME_FAIL = BIT7;

const char* MQTT_TOPIC_11 = "IMM/P010-left/Metric/injectionTime"; 
const char* MQTT_TOPIC_12 = "IMM/P010-left/Metric/injectionCycle"; 
const char* MQTT_TOPIC_13 = "IMM/P010-left/Metric/operationTime"; 
const char* MQTT_TOPIC_14 = "IMM/P010-left/Metric/counterShot"; 
const char* MQTT_TOPIC_15 = "IMM/P010-left/Metric/shiftNumber"; 
const char* MQTT_TOPIC_16 = "IMM/P010-left/Metric/badProductFront"; 
const char* MQTT_TOPIC_17 = "IMM/P010-left/Metric/badProductBack"; 


const char* MQTT_TOPIC_21 = "IMM/P010-right/Metric/injectionTime"; 
const char* MQTT_TOPIC_22 = "IMM/P010-right/Metric/injectionCycle"; 
const char* MQTT_TOPIC_23 = "IMM/P010-right/Metric/operationTime"; 
const char* MQTT_TOPIC_24 = "IMM/P010-right/Metric/counterShot"; 
const char* MQTT_TOPIC_25 = "IMM/P010-right/Metric/shiftNumber"; 
const char* MQTT_TOPIC_26 = "IMM/P010-right/Metric/badProductFront"; 
const char* MQTT_TOPIC_27 = "IMM/P010-right/Metric/badProductBack"; 
 
const char* CYCLE_TIME_TOPIC = "IMM/P010/CycleMessage"; 
const char* TEST_TOPIC = "IMM/P010"; 
const char* OPEN_TIME_TOPIC = "IMM/P010/OpenTime"; 
const char* MACHINE_STATUS_TOPIC_1 = "IMM/P010-left/Metric/machineStatus";
const char* MACHINE_STATUS_TOPIC_2 = "IMM/P010-right/Metric/machineStatus";
const char* MACHINE_LWT_TOPIC = "IMM/P010/LWT";
const char* DA_MESSAGE_TOPIC = "IMM/P010/DAMess"; 
const char* CONFIGURATION_TOPIC_LEFT = "IMM/P010-left/CollectingData";
const char* CONFIGURATION_TOPIC_RIGHT = "IMM/P010-right/CollectingData";
const char* SYNC_TIME_TOPIC = "IMM/P010/SyncTime";
const char* FEEDBACK_TOPIC = "IMM/P010/Feedback";
const char* MOLD_ID_DEFAULT = "NotConfigured";
const char* RECONNECT_BROKER_TIMER = "RECONNECT_BROKER_TIMER";
const char* UPDATE_MQTT_TIMER = "UPDATE_MQTT_TIMER";
const char* OPEN_CHECK_TIMER = "OPEN_CHECK_TIMER";
const char* INITIATE_TASK_TIMER = "INITIATE_TASK_TIMER";
const char* SHIFT_REBOOT_TIMER = "SHIFT_REBOOT_TIMER";
const char* STATUS_TIMER_1 = "STATUS_TIMER_1";
const char* STATUS_TIMER_2 = "STATUS_TIMER_2";
const char* NG1_TIMER = "NG1_TIMER";

const char* RECONNECT_TIMER = "RECONNECT_TIMER";
const char* BOOT_CONNECT_TIMER ="BOOT_CONNECT_TIMER"; 

static int   cycle_id_1;
static int   cycle_id_2;
static int   NG1;
static int   NG2;
static int   NG3;
static int   NG4;
static int   total_operation_time_1;
static int   total_operation_time_2;
static int   DA_cmd_1;
static int   DA_cmd_2;
static int   shift;
static int   reconnect_time;
static int   error_time_local;
static int   reboot_timer_flag;
static int64_t   remain_time;
static bool  error_sd_card;
static bool  error_rtc;
static bool  boot_to_reconnect;
static bool  idle_trigger_1;
static bool  idle_trigger_2;
static bool  panic_stop_1;
static bool  panic_stop_2;
static cJSON *my_json;
static char  CURRENT_CYCLE_FILE[50] = "/sdcard/c1090422.csv";
static char  CURRENT_STATUS_FILE[50] = "/sdcard/s1090422.csv";
//static char *message_lwt = NULL; 

/*----------------------------------------------------------------------*/
esp_err_t start_file_server(const char *base_path);
/*----------------------------------------------------------------------*/
/**
 * @brief Delete the task that call this fuction
 *
 */
/*----------------------------------------------------------------------*/

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void delete_task()
{
  vTaskDelete(NULL);
}
/*----------------------------------------------------------------------*/
/**
 * @brief Write content to file in SD card in append+ mode
 *
 * @param content Content to write
 * @param file_path File path to write
 */
void write_to_sd(char content[],char file_path[])
{
  FILE* f = fopen(file_path, "a+");
  if (f == NULL) 
    {
      ESP_LOGE(TAG, "Failed to open file for writing --> Restart ESP");      
      esp_restart();
      return;
    }
  int i = 0;
  while (content[i] != NULL) i++;
  
  char buff[i+1];
  for (int j = 0; j<i+1;j++)
    {
      buff[j] = content[j];
    }
  fprintf(f,buff);
  fprintf(f,"\n");
  fclose(f);
  ESP_LOGI(TAG, "File written"); 
}
/*----------------------------------------------------------------------*/
/**
 * @brief Mount SD card using SPI bus
 *
 */
static void sdcard_mount()
{
    /*sd_card part code*/
    esp_vfs_fat_sdmmc_mount_config_t mount_config = 
    {
      .format_if_mount_failed = true,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t* card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to initialize bus.");
      }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;
    esp_err_t err = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if(err != ESP_OK)
    {
      error_sd_card = true;
      char mess_fb[200];
      sprintf(mess_fb,"{%cMess%c:%d}",34,34,SDcardfail);

      if(esp_mqtt_client_publish(client,FEEDBACK_TOPIC,mess_fb,0,1,0) == -1)
      {
        esp_mqtt_client_enqueue(client,FEEDBACK_TOPIC,mess_fb,0,1,0,1);
      }
      if (err == ESP_FAIL) 
      {
          ESP_LOGE(TAG, "Failed to mount filesystem. "
              "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
      } 
      else 
      {
        ESP_LOGE(TAG, "Failed to initialize the card. "
            "Make sure SD card lines have pull-up resistors in place.");
      }
    }
    else if (err == ESP_OK)
    {
      sdmmc_card_print_info(stdout, card);
      ESP_ERROR_CHECK(start_file_server("/sdcard"));
    } 
}
/*----------------------------------------------------------------------*/
static esp_err_t unmount_card(const char* base_path, sdmmc_card_t* card)
{
    esp_err_t err = esp_vfs_fat_sdcard_unmount(base_path, card);

    ESP_ERROR_CHECK(err);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    err = spi_bus_free(host.slot);

    ESP_ERROR_CHECK(err);

    return err;
}
/*----------------------------------------------------------------------*/
/**
 * @brief Luu du lieu vao bo nho nvs cua ESP32
 *
 * @param value2write con tro toi gia tri duoc ghi
 * @param keyvalue ten bien can luu trong nvs
 */
esp_err_t save_value_nvs(int *value2write,char *keyvalue)
{
    nvs_handle_t my_handle;
    esp_err_t err;
    
    err = nvs_open(STORAGE_NAMESPACE,NVS_READWRITE, &my_handle);
    
    switch (err) {
            case ESP_OK:
                
                err = nvs_set_i32(my_handle,keyvalue,*value2write);            
                err = nvs_commit(my_handle);
                nvs_close(my_handle);
                return err;
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG,"The value is not initialized yet!\n");
               
                err = nvs_set_i32(my_handle,keyvalue,*value2write); 
               
                err = nvs_commit(my_handle);
                nvs_close(my_handle);                
                return err;
                break;
            default :
                nvs_close(my_handle);
                

                return err;
        }
}

/*----------------------------------------------------------------------*/
/**
 * @brief Doc du lieu tu bo nho nvs cua ESP32 va luu vao bien value2read
 *
 * @param value2read con tro toi gia tri can duoc load vao
 * @param keyvalue ten bien trong nvs
 */
esp_err_t load_value_nvs(int *value2read,char *keyvalue)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(STORAGE_NAMESPACE,NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
      return err;
    err = nvs_get_i32(my_handle,keyvalue,value2read);
    switch (err) 
      {
        case ESP_OK:
            nvs_close(my_handle);
            return ESP_OK;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG,"The value is not initialized yet!\n");
            err = nvs_set_i32(my_handle,keyvalue,*value2read);
            err = nvs_commit(my_handle);
            nvs_close(my_handle);
            return err;
            break;
        default :
            nvs_close(my_handle);
            return err;
      }
}
/*----------------------------------------------------------------------*/
void set_clock(struct tm *timeinfo)
{
  struct tm time2set = {
    .tm_year = timeinfo->tm_year,
    .tm_mon = timeinfo->tm_mon,
    .tm_mday = timeinfo->tm_mday,
    .tm_hour = timeinfo->tm_hour,
    .tm_min = timeinfo->tm_min,
    .tm_sec = timeinfo->tm_sec
  };
  if (ds1307_set_time(&rtc_i2c, &time2set) != ESP_OK) {
        ESP_LOGE(pcTaskGetTaskName(0), "Could not set time.");
        while (1) { vTaskDelay(1); }
    }
    ESP_LOGI(pcTaskGetTaskName(0), "Set date time done");

  delete_task();
}
/*----------------------------------------------------------------------*/
static int calculate_diff_time(struct tm start_time, struct tm end_time)
{
  int diff_time;
  int diff_hour;
  int diff_min;
  int diff_sec;
  int e_sec = 0;
  int e_min = 0;

  
  if (end_time.tm_sec < start_time.tm_sec)
  {
    diff_sec = end_time.tm_sec + 60 - start_time.tm_sec;
    e_sec = 1;
  }
  else
  {
    diff_sec = end_time.tm_sec - start_time.tm_sec;
  }

  if (end_time.tm_min < start_time.tm_min)
  {
    diff_min = end_time.tm_min - e_sec + 60 - start_time.tm_min;
    e_min = 1;
  }
  else
  {
    diff_min = end_time.tm_min - e_sec - start_time.tm_min;
  }

  if (end_time.tm_hour < start_time.tm_hour)
  {
    diff_hour = end_time.tm_hour - e_min + 24 - start_time.tm_hour;
  }
  else
  {
    diff_hour = end_time.tm_hour - e_min - start_time.tm_hour;
  }

  diff_time = (diff_hour*3600 + diff_min*60 + diff_sec)*1000;

  return diff_time;
}
/*----------------------------------------------------------------------*/
void get_tomorrow(struct tm *tomorrow,struct tm *today)
{
  int year = today->tm_year;
  int mon = today->tm_mon;
  int mday = today->tm_mday;
  int leap_year = year%4;

  tomorrow->tm_year = today->tm_year;
  tomorrow->tm_mon = today->tm_mon;
  tomorrow->tm_mday = today->tm_mday + 1;

  if ((mday == 31)&&(mon == 1))
    {
      tomorrow->tm_mon = 2;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 28)&&(mon == 2))
    {
      if (leap_year != 0)
        {
          tomorrow->tm_mon = 3;
          tomorrow->tm_mday = 1;
        }
      else
        {
          tomorrow->tm_mon = 2;
          tomorrow->tm_mday = 29;
        }
      return;
    }
  if ((mday == 29)&&(mon == 2))
    {
      tomorrow->tm_mon = 3;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 3))
    {
      tomorrow->tm_mon = 4;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 30)&&(mon == 4))
    {
      tomorrow->tm_mon = 5;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 5))
    {
      tomorrow->tm_mon = 6;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 30)&&(mon == 6))
    {
      tomorrow->tm_mon = 7;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 7))
    {
      tomorrow->tm_mon = 8;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 8))
    {
      tomorrow->tm_mon = 9;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 30)&&(mon == 9))
    {
      tomorrow->tm_mon = 10;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 10))
    {
      tomorrow->tm_mon = 11;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 30)&&(mon == 11))
    {
      tomorrow->tm_mon = 12;
      tomorrow->tm_mday = 1;
      return;
    }
  if ((mday == 31)&&(mon == 12))
    {
      tomorrow->tm_mon = 1;
      tomorrow->tm_mday = 1;
      tomorrow->tm_year = tomorrow->tm_year + 1;
      return;
    }
  return;
}
/*-------------------------------------------------------------------*/
void get_previous_day(struct tm *previous,struct tm *today)
{
  int year = today->tm_year;
  int mon = today->tm_mon;
  int mday = today->tm_mday;
  int leap_year = year%4;

  previous->tm_year = today->tm_year;
  previous->tm_mon = today->tm_mon;
  previous->tm_mday = today->tm_mday -1;

  if ((mday == 1)&&(mon == 1))
    {
      previous->tm_mon = 12;
      previous->tm_mday = 31;
      previous->tm_year = previous->tm_year -1; 
      return;
    }
  if ((mday ==1)&&(mon == 2))
    {
      previous->tm_mon = 1;
      previous->tm_mday = 31;
      return;
      
    }
  if ((mday == 1)&&(mon == 3))
    {
      if (leap_year != 0)
        {
          previous->tm_mon = 2;
          previous->tm_mday = 28;
        }
      else
        {
          previous->tm_mon = 2;
          previous->tm_mday = 29;
        }
      return;
    }
  if ((mday == 1)&&(mon == 4))
    {
      previous->tm_mon = 3;
      previous->tm_mday = 31;
      return;
    }
  if ((mday == 1)&&(mon == 5))
    {
      previous->tm_mon = 4;
      previous->tm_mday = 30;
      return;
    }
  if ((mday == 1)&&(mon == 6))
    {
      previous->tm_mon = 5;
      previous->tm_mday = 31;
      return;
    }
  if ((mday == 1)&&(mon == 7))
    {
      previous->tm_mon = 6;
      previous->tm_mday = 30;
      return;
    }
  if ((mday == 1)&&(mon == 8))
    {
      previous->tm_mon = 7;
      previous->tm_mday = 31;
      return;
    }
  if ((mday == 1)&&(mon == 9))
    {
      previous->tm_mon = 8;
      previous->tm_mday = 31;
      return;
    }
  if ((mday == 1)&&(mon == 10))
    {
      previous->tm_mon = 9;
      previous->tm_mday = 30;
      return;
    }
  if ((mday == 1)&&(mon == 11))
    {
      previous->tm_mon = 31;
      previous->tm_mday = 10;
      return;
    }
  if ((mday == 1)&&(mon == 12))
    {
      previous->tm_mon = 30;
      previous->tm_mday = 11;
      return;
    }

  return;
}
/*----------------------------------------------------------------------*/
void restart_esp()
{
  cycle_id_1 = 0;
  save_value_nvs(&cycle_id_1,"cycle_id_1");
  cycle_id_2 = 0;
  save_value_nvs(&cycle_id_2,"cycle_id_2");
  NG1 = 0;
  save_value_nvs(&NG1,"NG1");
  NG2 = 0;
  save_value_nvs(&NG2,"NG2");
  NG3 = 0;
  save_value_nvs(&NG3,"NG3");
  NG4 = 0;
  save_value_nvs(&NG4,"NG4");
  total_operation_time_1 = 0;
  save_value_nvs(&total_operation_time_1,"total_operation_time_1");
  total_operation_time_2= 0;
  save_value_nvs(&total_operation_time_2,"total_operation_time_2");
  esp_restart();
}
static char* generate_lwt_json()
{
  cJSON *root;
  char *jSON_lwt;
  char message_lwt[100];
  if (error_rtc == false)          
  ds1307_get_time(&rtc_i2c, &lwt_time);
  //sprintf(message_lwt,"%04d-%02d-%02dT%02d:%02d:%02d",lwt_time.tm_year, lwt_time.tm_mon,lwt_time.tm_mday, lwt_time.tm_hour,lwt_time.tm_min, lwt_time.tm_sec);
  sprintf(message_lwt,"[{%cmachineId%c: %cP010-left%c},{%cmachineId%c: %cP010-rigth%c}]",34,34,34,34,34,34,34,34);
  root = cJSON_CreateObject();
  //cJSON_AddNumberToObject(root,"machineStatus",5);  
  //cJSON_AddStringToObject(root,"machines",message_lwt);    
  //cJSON_AddStringToObject(root,"P010-left","P010-right");  
  // char array[2] = ["P010-left","P010-right"];
  // cJSON_AddArrayToObject(root, array);
  jSON_lwt = cJSON_Print(root);
  return jSON_lwt;
   
}
/*----------------------------------------------------------------------*/
static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    // Mount SD card
    sdcard_mount();  
    double last_cycle_time_1;
    double last_injectionTime_1;
    cycle_id_1 = 0;
    total_operation_time_1 = 0;
    double last_cycle_time_2;
    double last_injectionTime_2;
    cycle_id_2 = 0;
    total_operation_time_2 = 0;
    bool open_door_flag_1 = false;
    bool open_door_flag_2 = false;            
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) 
        {          
          if ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_CYCLE_1) && (open_door_flag_1 == true)&&(panic_stop_1 == false))
           {
              ESP_LOGI("GPIO","Mold 1-2 is already closed %d",GPIO_INPUT_CYCLE_1);  
              xTimerStop(soft_timer_handle_5,10);  // Timer check idle status
              xTimerStart(soft_timer_handle_5,10);
              // Lay thoi gian chu ki ep            
              timer_pause(TIMER_GROUP_0, TIMER_0);
              timer_get_counter_time_sec(TIMER_GROUP_0,TIMER_0,&last_cycle_time_1);            							
              timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0x00000000ULL);
              timer_start(TIMER_GROUP_0, TIMER_0);    

              //Chay timer tinh thoi gian ep            
              timer_start(TIMER_GROUP_0, TIMER_1);      
              // TWDT
              esp_task_wdt_reset();

              // Chong nhieu chu ki ep
              if (last_cycle_time_1 > 50.0 && DA_cmd_1 != 1)
              {
                cycle_id_1++;                        
                save_value_nvs(&cycle_id_1,"cycle_id_1");
                cycle_info_1.counter_shot = cycle_id_1;                              
                cycle_info_1.mode_working = 0;  // May hoat dong o che do ban tu dong
                cycle_info_1.cycle_time = last_cycle_time_1;
                total_operation_time_1 += (int)last_cycle_time_1;                
                save_value_nvs(&total_operation_time_1,"total_operation_time_1");
                cycle_info_1.operationTime = total_operation_time_1;
                // luu thoi gian ep (inject_time_1 la thoi gian ep)
                cycle_info_1.injectionTime = last_injectionTime_1;
                
            
                if (idle_trigger_1 == false)
                { 
                  sprintf(message_info_1.message_mqtt,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Run,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
                  esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_1,message_info_1.message_mqtt,0,1,1);                 

                  sprintf(message_info_1.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec,Run);
                  write_to_sd(message_info_1.message_text,CURRENT_STATUS_FILE);
                }
                else
                {
                  idle_trigger_1 = false;
                }
                  ESP_LOGI("MQTT_DAQ","Publishing MQTT Unit 1");   
                  sprintf(message_info_1.mqtt_injectionTime,"[{%cname%c:%cinjectionTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.injectionTime,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                               
                  sprintf(message_info_1.mqtt_injectionCycle,"[{%cname%c:%cinjectionCycle%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.cycle_time,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
                  sprintf(message_info_1.mqtt_operationTime,"[{%cname%c:%coperationTime%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.operationTime,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                               
                  sprintf(message_info_1.mqtt_counterShot,"[{%cname%c:%ccounterShot%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.counter_shot,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
                  sprintf(message_info_1.mqtt_shiftNumber,"[{%cname%c:%cshiftNumber%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,shift,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);  
                 
                  esp_mqtt_client_publish(client,MQTT_TOPIC_11,message_info_1.mqtt_injectionTime,0,1,1);
                  esp_mqtt_client_publish(client,MQTT_TOPIC_12,message_info_1.mqtt_injectionCycle,0,1,1);
                  esp_mqtt_client_publish(client,MQTT_TOPIC_13,message_info_1.mqtt_operationTime,0,1,1);
                  esp_mqtt_client_publish(client,MQTT_TOPIC_14,message_info_1.mqtt_counterShot,0,1,1);
                  esp_mqtt_client_publish(client,MQTT_TOPIC_15,message_info_1.mqtt_shiftNumber,0,1,1);
                  
                  // TWDT
                  esp_task_wdt_reset();     

                  ESP_LOGI("SD card","File writing Unit 1");
                  sprintf(message_info_1.message_text,"%f,%f,%04d-%02d-%02dT%02d:%02d:%02d",cycle_info_1.injectionTime,cycle_info_1.cycle_time,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec);                    
                  write_to_sd(message_info_1.message_text,CURRENT_CYCLE_FILE);
                  //TWDT
                  esp_task_wdt_reset();
              }                
              //Lay thoi gian bat dau chu ky
              if (error_rtc == false)
              {
                ds1307_get_time(&rtc_i2c, &inject_time_1);
                ESP_LOGI("GPIO","Event Cycle Unit 1 %f %02d:%02d:%02d",cycle_info_1.cycle_time,inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec); 
                //TWDT
                esp_task_wdt_reset();
                }
              else
                {
                  ESP_LOGE("GPIO","Error RTC, cycle info save on default file: Event Cycle mould 1 %f %02d:%02d:%02d",cycle_info_1.cycle_time,inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec); 
                }     
                open_door_flag_1 = false;     
              
           }
           else if  (!(bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_CYCLE_1) && (open_door_flag_1 == false)&&(panic_stop_1==false))
           {
             ESP_LOGI("GPIO","Mold 1-2 is already opened %d",GPIO_INPUT_CYCLE_1);
            // Lay thoi gian ep
            timer_pause(TIMER_GROUP_0, TIMER_1);
            timer_get_counter_time_sec(TIMER_GROUP_0,TIMER_1,&last_injectionTime_1);            							
            timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0x00000000ULL); 
            open_door_flag_1 = true;
           }
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_CYCLE_2)&& (open_door_flag_2 == true)&&(panic_stop_2 == false))  
           {
            ESP_LOGI("GPIO","Mold 3-4 is already closed %d",GPIO_INPUT_CYCLE_2);  
            xTimerStop(soft_timer_handle_5,10);  // Timer check idle status
            xTimerStart(soft_timer_handle_5,10);
            // Lay thoi gian chu ki ep            
            timer_pause(TIMER_GROUP_1, TIMER_0);
            timer_get_counter_time_sec(TIMER_GROUP_1,TIMER_0,&last_cycle_time_2);            							
            timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL);
            timer_start(TIMER_GROUP_1, TIMER_0);

            //Chay timer tinh thoi gian ep            
            timer_start(TIMER_GROUP_1, TIMER_1);                         
            // TWDT
            esp_task_wdt_reset(); 

            // Chong nhieu chu ki ep
            if (last_cycle_time_2 > 50.0 && DA_cmd_2 != 1)
            {
              cycle_id_2++;                        
              save_value_nvs(&cycle_id_2,"cycle_id_2");
              cycle_info_2.counter_shot = cycle_id_2;                           
              cycle_info_2.mode_working = 0;  // May hoat dong o che do ban tu dong
              cycle_info_2.cycle_time = last_cycle_time_2;
              total_operation_time_2 += (int)last_cycle_time_2;                
              save_value_nvs(&total_operation_time_2,"total_operation_time_2");
              cycle_info_2.operationTime= total_operation_time_2;
              // luu thoi gian ep (inject_time_2 la thoi gian ep)
              cycle_info_2.injectionTime = last_injectionTime_2;
              
          
              if (idle_trigger_2 == false)
              { 
                sprintf(message_info_2.message_mqtt,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Run,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
                esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_2,message_info_2.message_mqtt,0,1,1);                 
        
                sprintf(message_info_2.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec,Run);
                write_to_sd(message_info_2.message_text,CURRENT_STATUS_FILE);
              }
              else
              {
                idle_trigger_2 = false;
              }
              ESP_LOGI("MQTT_DAQ","Publishing MQTT Unit 2");    
              sprintf(message_info_2.mqtt_injectionTime,"[{%cname%c:%cinjectionTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.injectionTime,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                               
              sprintf(message_info_2.mqtt_injectionCycle,"[{%cname%c:%cinjectionCycle%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.cycle_time,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
              sprintf(message_info_2.mqtt_operationTime,"[{%cname%c:%coperationTime%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.operationTime,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                               
              sprintf(message_info_2.mqtt_counterShot,"[{%cname%c:%ccounterShot%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.counter_shot,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
              sprintf(message_info_2.mqtt_shiftNumber,"[{%cname%c:%cshiftNumber%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,shift,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);  
             
              esp_mqtt_client_publish(client,MQTT_TOPIC_21,message_info_2.mqtt_injectionTime,0,1,1);
              esp_mqtt_client_publish(client,MQTT_TOPIC_22,message_info_2.mqtt_injectionCycle,0,1,1);
              esp_mqtt_client_publish(client,MQTT_TOPIC_23,message_info_2.mqtt_operationTime,0,1,1);
              esp_mqtt_client_publish(client,MQTT_TOPIC_24,message_info_2.mqtt_counterShot,0,1,1);
              esp_mqtt_client_publish(client,MQTT_TOPIC_25,message_info_2.mqtt_shiftNumber,0,1,1);
              
              ESP_LOGI("SD card","File writing Unit 2");
              sprintf(message_info_2.message_text,"%f,%f,%04d-%02d-%02dT%02d:%02d:%02d",cycle_info_2.injectionTime,cycle_info_2.cycle_time,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec);                    
              write_to_sd(message_info_2.message_text,CURRENT_CYCLE_FILE);              
            }                
            //Lay thoi gian bat dau chu ky
            if (error_rtc == false)
              {
              ds1307_get_time(&rtc_i2c, &inject_time_2);
              ESP_LOGI("GPIO","Event Cycle Unit 2 %f %02d:%02d:%02d",cycle_info_2.cycle_time,inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec); 
             
              }
            else
              {
                ESP_LOGE("GPIO","Error RTC, cycle info save on default file: Event Cycle mould 2 %f %02d:%02d:%02d",cycle_info_2.cycle_time,inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec); 
              }          
            open_door_flag_2 = false; 
           }
           else if  (!(bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_CYCLE_2) && (open_door_flag_2 == false)&&(panic_stop_2 == false))
           {
              ESP_LOGI("GPIO","Mold 3-4 is already opened %d",GPIO_INPUT_CYCLE_2);
              // Lay thoi gian ep
              timer_pause(TIMER_GROUP_1, TIMER_1);
              timer_get_counter_time_sec(TIMER_GROUP_1,TIMER_1,&last_injectionTime_2);       							
              timer_set_counter_value(TIMER_GROUP_1, TIMER_1, 0x00000000ULL); 
              open_door_flag_2 = true;
           }           
           else if  (!(bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_SETUP_1) && (panic_stop_1 == false))
           {                        
            ESP_LOGE("Panic alert","Set up Unit 1 begin");   
            xTimerStop(soft_timer_handle_5,10);
            sprintf(message_info_1.message_mqtt,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Setup,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
            esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_1,message_info_1.message_mqtt,0,1,1);                 
            // Write "Setup" mode to SDcard - Unit 1
            sprintf(message_info_1.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec,Setup);
            write_to_sd(message_info_1.message_text,CURRENT_STATUS_FILE);     
            panic_stop_1 = true;       
           } 
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_SETUP_1) && (panic_stop_1 == true))
           {             
             ESP_LOGE("Panic alert","Set up Unit 1 finish");   
             panic_stop_1 = false;
           }
           else if  (!(bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_SETUP_2) && (panic_stop_2 == false))
           {
            ESP_LOGE("Panic alert","Set up Unit 2 begin");   
            xTimerStop(soft_timer_handle_8,10);
            sprintf(message_info_2.message_mqtt,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Setup,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
            esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_2,message_info_2.message_mqtt,0,1,1);                 
            // Write "Setup" mode to SDcard - Unit 1
            sprintf(message_info_2.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec,Setup);
            write_to_sd(message_info_2.message_text,CURRENT_STATUS_FILE);     
            panic_stop_2 = true;   
           } 
          else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_SETUP_2) && (panic_stop_2 == true))
           {              
              ESP_LOGE("Panic alert","Set up Unit 2 finish");   
              panic_stop_2 = false;               
           }     
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_BAD_1))
           {            
            vTaskDelay(100/portTICK_PERIOD_MS);  
            while(gpio_get_level(GPIO_INPUT_BAD_1)==0) vTaskDelay(100/portTICK_PERIOD_MS); 
            // vTaskDelay(100/portTICK_PERIOD_MS);                    
            NG1++;
            save_value_nvs(&NG1,"NG1");
            cycle_info_1.NGFront = NG1;            
            ESP_LOGE("Event NG","Eject event of Unit 1-front: %d",NG1);
            sprintf(message_info_1.mqtt_badProductFront,"[{%cname%c:%cbadProduct-1%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.NGFront,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                             
            //esp_mqtt_client_publish(client,MQTT_TOPIC_16,message_info_1.mqtt_badProductFront,0,1,1);                          
           }
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_BAD_2))
           {
            vTaskDelay(100/portTICK_PERIOD_MS);  
            while(gpio_get_level(GPIO_INPUT_BAD_2)==0) vTaskDelay(100/portTICK_PERIOD_MS); 
            // vTaskDelay(100/portTICK_PERIOD_MS); 
            NG2++;
            save_value_nvs(&NG2,"NG2");
            cycle_info_1.NGBack = NG2; 
            ESP_LOGE("Event NG","Eject event of Unit 1-back: %d",NG2);
            sprintf(message_info_1.mqtt_badProductBack,"[{%cname%c:%cbadProduct-2%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.NGBack,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
            //esp_mqtt_client_publish(client,MQTT_TOPIC_17,message_info_1.mqtt_badProductBack,0,1,1);        
            
           }
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_BAD_3))
           {
            vTaskDelay(100/portTICK_PERIOD_MS);  
            while(gpio_get_level(GPIO_INPUT_BAD_3)==0) vTaskDelay(100/portTICK_PERIOD_MS); 
            // vTaskDelay(100/portTICK_PERIOD_MS); 
            NG3++;
            save_value_nvs(&NG3,"NG3");
            cycle_info_2.NGFront = NG3;
            ESP_LOGE("Event NG","Eject event of Unit 2-front: %d",NG3);
            sprintf(message_info_2.mqtt_badProductFront,"[{%cname%c:%cbadProduct-1%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.NGFront,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                             
           // esp_mqtt_client_publish(client,MQTT_TOPIC_26,message_info_2.mqtt_badProductFront,0,1,1);
            
           }
           else if  ((bool)gpio_get_level(io_num) && (io_num == GPIO_INPUT_BAD_4))
           {
            vTaskDelay(100/portTICK_PERIOD_MS);  
            while(gpio_get_level(GPIO_INPUT_BAD_4)==0) vTaskDelay(100/portTICK_PERIOD_MS); 
            // vTaskDelay(100/portTICK_PERIOD_MS);  
            NG4++;
            save_value_nvs(&NG4,"NG4");
            cycle_info_2.NGBack = NG4;
            ESP_LOGE("Event NG","Eject event of Unit 2-back: %d",NG4);
            sprintf(message_info_2.mqtt_badProductBack,"[{%cname%c:%cbadProduct-2%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.NGBack,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
            //esp_mqtt_client_publish(client,MQTT_TOPIC_27,message_info_2.mqtt_badProductBack,0,1,1);              
           }     
        }
         
        vTaskDelay(100/portTICK_PERIOD_MS); 
    }
}

/*----------------------------------------------------------------------*/
// static void gpio_task(void* arg) 
// { 
//   double last_cycle_time_1;
//   double last_injectionTime_1;
//   cycle_id_1 = 0;
//   double last_cycle_time_2;
//   double last_injectionTime_2;
//   cycle_id_2 = 0;
//   bool open_door_flag_1 = false;
//   bool open_door_flag_2 = false;
//   //TWDT
//   esp_task_wdt_reset();
//   for (;;)
//     {         
//     if (xQueueReceive(button_events, &ev, 1000/portTICK_PERIOD_MS)) 
//       { 
//             if ((ev.pin == GPIO_INPUT_CYCLE_1) && (ev.event == BUTTON_DOWN) && (open_door_flag_1 == false))
//               {    
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;        
//                 ESP_LOGI("GPIO","Mold 1-2 is already opened %d",GPIO_INPUT_CYCLE_1);
//                 // Lay thoi gian ep
//                 timer_pause(TIMER_GROUP_0, TIMER_1);
//                 timer_get_counter_time_sec(TIMER_GROUP_0,TIMER_1,&last_injectionTime_1);            							
//                 timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0x00000000ULL); 
                
//                 open_door_flag_1 = true;  
//                 //TWDT
//                 esp_task_wdt_reset();  
//               }
//             else if ((ev.pin == GPIO_INPUT_CYCLE_2) && (ev.event == BUTTON_DOWN) && (open_door_flag_2 == false))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 ESP_LOGI("GPIO","Mold 3-4 is already opened %d",GPIO_INPUT_CYCLE_2);
//                 // Lay thoi gian ep
//                 timer_pause(TIMER_GROUP_1, TIMER_1);
//                 timer_get_counter_time_sec(TIMER_GROUP_1,TIMER_1,&last_injectionTime_2);       							
//                 timer_set_counter_value(TIMER_GROUP_1, TIMER_1, 0x00000000ULL); 
                
//                 open_door_flag_2 = true;
//                 // //TWDT
//                 esp_task_wdt_reset();                        

//               }    
//             else if ((ev.pin == GPIO_INPUT_CYCLE_1) && (ev.event == BUTTON_UP) && (open_door_flag_1 == true))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 ESP_LOGI("GPIO","Mold 1-2 is already closed %d",GPIO_INPUT_CYCLE_1);  
//                 xTimerStop(soft_timer_handle_5,10);  // Timer check idle status
//                 xTimerStart(soft_timer_handle_5,10);
//                 // Lay thoi gian chu ki ep            
//                 timer_pause(TIMER_GROUP_0, TIMER_0);
//                 timer_get_counter_time_sec(TIMER_GROUP_0,TIMER_0,&last_cycle_time_1);            							
//                 timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0x00000000ULL);
//                 timer_start(TIMER_GROUP_0, TIMER_0);
//                 // Chay timer tinh thoi gian ep            
//                 timer_start(TIMER_GROUP_0, TIMER_1);  
//                 open_door_flag_1 = false;  
//                 // TWDT
//                 esp_task_wdt_reset();

//                 // Chong nhieu chu ki ep
//                 if (last_cycle_time_1 > 50.0 && DA_cmd_1 != 1)
//                 {
//                   cycle_id_1++;                        
//                   save_value_nvs(&cycle_id_1,"cycle_id_1");
//                   cycle_info_1.counter_shot = cycle_id_1;  
//                   cycle_info_1.NGFront = NG1;
//                   cycle_info_1.NGBack = NG2;             
//                   cycle_info_1.mode_working = 0;  // May hoat dong o che do ban tu dong
//                   cycle_info_1.cycle_time = last_cycle_time_1;
//                   cycle_info_1.operationTime += last_cycle_time_1;
//                   // luu thoi gian ep (inject_time_1 la thoi gian ep)
//                   cycle_info_1.injectionTime = last_injectionTime_1;
              
//                   if (idle_trigger == false)
//                   {         
//                     sprintf(message_info_1.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec,OnProduction);
//                     write_to_sd(message_info_1.message_text,CURRENT_STATUS_FILE);
//                   }
//                   else
//                   {
//                     idle_trigger = false;
//                   }
//                     ESP_LOGI("MQTT_DAQ","Publishing MQTT mould 1");   
//                     sprintf(message_info_1.mqtt_injectionTime,"[{%cname%c:%cinjectionTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.injectionTime,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                               
//                     sprintf(message_info_1.mqtt_injectionCycle,"[{%cname%c:%cinjectionCycle%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.cycle_time,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
//                     sprintf(message_info_1.mqtt_operationTime,"[{%cname%c:%coperationTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.operationTime,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                               
//                     sprintf(message_info_1.mqtt_counterShot,"[{%cname%c:%ccounterShot%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.counter_shot,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
//                     sprintf(message_info_1.mqtt_shiftNumber,"[{%cname%c:%cshiftNumber%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,shift,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);  
//                     sprintf(message_info_1.mqtt_badProductFront,"[{%cname%c:%cbadProduct-1%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.NGFront,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34);                             
//                     sprintf(message_info_1.mqtt_badProductBack,"[{%cname%c:%cbadProduct-2%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_1.NGBack,34,34,34,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour, inject_time_1.tm_min, inject_time_1.tm_sec,34); 
                
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_11,message_info_1.mqtt_injectionTime,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_12,message_info_1.mqtt_injectionCycle,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_13,message_info_1.mqtt_operationTime,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_14,message_info_1.mqtt_counterShot,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_15,message_info_1.mqtt_shiftNumber,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_16,message_info_1.mqtt_badProductFront,0,1,1);
//                     esp_mqtt_client_publish(client,MQTT_TOPIC_17,message_info_1.mqtt_badProductBack,0,1,1);        
                  
//                     // TWDT
//                     esp_task_wdt_reset();     

//                     ESP_LOGI("SD card","File writing mould 1");
//                     sprintf(message_info_1.message_text,"%f,%f,%04d-%02d-%02dT%02d:%02d:%02d",cycle_info_1.injectionTime,cycle_info_1.cycle_time,inject_time_1.tm_year, inject_time_1.tm_mon,inject_time_1.tm_mday, inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec);                    
//                     write_to_sd(message_info_1.message_text,CURRENT_CYCLE_FILE);
//                     //TWDT
//                     esp_task_wdt_reset();
//                 }                
//                 //Lay thoi gian bat dau chu ky
//                 if (error_rtc == false)
//                 {
//                   ds1307_get_time(&rtc_i2c, &inject_time_1);
//                   ESP_LOGI("GPIO","Event Cycle mould 1 %f %02d:%02d:%02d",cycle_info_1.cycle_time,inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec); 
//                   //TWDT
//                   esp_task_wdt_reset();
//                  }
//                 else
//                   {
//                     ESP_LOGE("GPIO","Error RTC, cycle info save on default file: Event Cycle mould 1 %f %02d:%02d:%02d",cycle_info_1.cycle_time,inject_time_1.tm_hour,inject_time_1.tm_min, inject_time_1.tm_sec); 
//                   }          
//                 //TWDT
//                 esp_task_wdt_reset();
//               }           
//           //}
//           //aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa        
//             else if ((ev.pin == GPIO_INPUT_CYCLE_2) && (ev.event == BUTTON_UP) && (open_door_flag_2 == true))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 ESP_LOGI("GPIO","Mold 3-4 is already closed %d",GPIO_INPUT_CYCLE_2);  
//                 xTimerStop(soft_timer_handle_5,10);  // Timer check idle status
//                 xTimerStart(soft_timer_handle_5,10);
//                 // Lay thoi gian chu ki ep            
//                 timer_pause(TIMER_GROUP_1, TIMER_0);
//                 timer_get_counter_time_sec(TIMER_GROUP_1,TIMER_0,&last_cycle_time_2);            							
//                 timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL);
//                 timer_start(TIMER_GROUP_1, TIMER_0);
//                 // Chay timer tinh thoi gian ep            
//                 timer_start(TIMER_GROUP_1, TIMER_1);  
//                 open_door_flag_2 = false; 
//                 // TWDT
//                 esp_task_wdt_reset(); 

//                 // Chong nhieu chu ki ep
//                 if (last_cycle_time_2 > 50.0 && DA_cmd_2 != 1)
//                 {
//                   cycle_id_2++;                        
//                   save_value_nvs(&cycle_id_2,"cycle_id_2");
//                   cycle_info_2.counter_shot = cycle_id_2;     
//                   cycle_info_2.NGFront = NG3;
//                   cycle_info_2.NGBack = NG4;          
//                   cycle_info_2.mode_working = 0;  // May hoat dong o che do ban tu dong
//                   cycle_info_2.cycle_time = last_cycle_time_2;
//                   cycle_info_2.operationTime += last_cycle_time_2;
//                   // luu thoi gian ep (inject_time_2 la thoi gian ep)
//                   cycle_info_2.injectionTime = last_injectionTime_2;
              
//                   if (idle_trigger == false)
//                   {         
//                     sprintf(message_info_2.message_text,"%04d-%02d-%02dT%02d:%02d:%02d,%d",inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec,OnProduction);
//                     write_to_sd(message_info_2.message_text,CURRENT_STATUS_FILE);
//                   }
//                   else
//                   {
//                     idle_trigger = false;
//                   }
//                   ESP_LOGI("MQTT_DAQ","Publishing MQTT mould 2");    
//                   sprintf(message_info_2.mqtt_injectionTime,"[{%cname%c:%cinjectionTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.injectionTime,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                               
//                   sprintf(message_info_2.mqtt_injectionCycle,"[{%cname%c:%cinjectionCycle%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.cycle_time,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
//                   sprintf(message_info_2.mqtt_operationTime,"[{%cname%c:%coperationTime%c,%cvalue%c:%f,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.operationTime,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                               
//                   sprintf(message_info_2.mqtt_counterShot,"[{%cname%c:%ccounterShot%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.counter_shot,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
//                   sprintf(message_info_2.mqtt_shiftNumber,"[{%cname%c:%cshiftNumber%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,shift,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);  
//                   sprintf(message_info_2.mqtt_badProductFront,"[{%cname%c:%cbadProduct-1%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.NGFront,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34);                             
//                   sprintf(message_info_2.mqtt_badProductBack,"[{%cname%c:%cbadProduct-2%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,cycle_info_2.NGBack,34,34,34,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour, inject_time_2.tm_min, inject_time_2.tm_sec,34); 
              
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_21,message_info_2.mqtt_injectionTime,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_22,message_info_2.mqtt_injectionCycle,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_23,message_info_2.mqtt_operationTime,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_24,message_info_2.mqtt_counterShot,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_25,message_info_2.mqtt_shiftNumber,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_26,message_info_2.mqtt_badProductFront,0,1,1);
//                   esp_mqtt_client_publish(client,MQTT_TOPIC_27,message_info_2.mqtt_badProductBack,0,1,1);        
                  
//                   // TWDT
//                   esp_task_wdt_reset();
//                   ESP_LOGI("SD card","File writing mould 2");
//                   sprintf(message_info_2.message_text,"%f,%f,%04d-%02d-%02dT%02d:%02d:%02d",cycle_info_2.injectionTime,cycle_info_2.cycle_time,inject_time_2.tm_year, inject_time_2.tm_mon,inject_time_2.tm_mday, inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec);                    
//                   write_to_sd(message_info_2.message_text,CURRENT_CYCLE_FILE);
                    
//                   //TWDT
//                   esp_task_wdt_reset();
//                 }                
//                 //Lay thoi gian bat dau chu ky
//                 if (error_rtc == false)
//                   {
//                   ds1307_get_time(&rtc_i2c, &inject_time_2);
//                   ESP_LOGI("GPIO","Event Cycle mould 2 %f %02d:%02d:%02d",cycle_info_2.cycle_time,inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec); 
//                   //TWDT
//                   esp_task_wdt_reset();
//                   }
//                 else
//                   {
//                     ESP_LOGE("GPIO","Error RTC, cycle info save on default file: Event Cycle mould 2 %f %02d:%02d:%02d",cycle_info_2.cycle_time,inject_time_2.tm_hour,inject_time_2.tm_min, inject_time_2.tm_sec); 
//                   }          
//                   //TWDT
//                   esp_task_wdt_reset();
//               }                                                 
//             else if ((ev.pin == GPIO_INPUT_BAD_1) && (ev.event == BUTTON_DOWN))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 NG1++;
//                 save_value_nvs(&NG1,"NG1");
//                 ESP_LOGE("Event NG","Eject event of mould 1-front: %d",NG1);                     
//                 //TWDT
//                 esp_task_wdt_reset();
//               }
//             else if ((ev.pin == GPIO_INPUT_BAD_2) && (ev.event == BUTTON_DOWN))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 NG2++;
//                 save_value_nvs(&NG2,"NG2");
//                 ESP_LOGE("Event NG","Eject event of mould 1-back: %d",NG2);
//                 //TWDT
//                 esp_task_wdt_reset();
//               }
//             else if ((ev.pin == GPIO_INPUT_BAD_3) && (ev.event == BUTTON_DOWN))
//               {
//                 // Khi co su kien thi set event_busy len 1
//               // event_busy =  true;
//                 NG3++;
//                 save_value_nvs(&NG3,"NG3");
//                 ESP_LOGE("Event NG","Eject event of mould 2-front: %d",NG3);
//                 //TWDT
//                 esp_task_wdt_reset();
//               }
//             else if ((ev.pin == GPIO_INPUT_BAD_4) && (ev.event == BUTTON_DOWN))
//               {
//                 // Khi co su kien thi set event_busy len 1
//                 //event_busy =  true;
//                 NG4++;
//                 save_value_nvs(&NG4,"NG4");
//                 ESP_LOGE("Event NG","Eject event of mould 2-back: %d",NG4);
//                 //TWDT
//                 esp_task_wdt_reset();
//               }                    
//           //   else
//           // {
//           //   // Khi khong co su kien thi reset event_busy
//           //  // ESP_LOGI("TAG","Other event pin");  
//           //   //event_busy =  false;
//           //   esp_task_wdt_reset();            
//           // }
//       }
//     // else
//     //   {
//     //     ESP_LOGI("TAG","Nothing happen");        
//     //     //TWDT
//     //     esp_task_wdt_reset();        
//     //   } 
//       vTaskDelay(100/portTICK_PERIOD_MS);    
//     }   
// }

/*----------------------------------------------------------------------*/
static void initiate_task(void* arg); 
/*----------------------------------------------------------------------*/
static void vSoftTimerCallback(TimerHandle_t xTimer)
{ //esp_task_wdt_add(NULL);
  if (pcTimerGetName(xTimer) == RECONNECT_BROKER_TIMER)
      {
        xTimerStop(soft_timer_handle_1,10);
        esp_mqtt_client_reconnect(client);
        //TWDT
        esp_task_wdt_reset();
        //esp_task_wdt_delete(NULL);
      }
  else if (pcTimerGetName(xTimer) == OPEN_CHECK_TIMER)
      {
        //esp_mqtt_client_publish(client,OPEN_TIME_ERROR_TOPIC,"Exceed Open Time",0,1,0);
      }
  else if (pcTimerGetName(xTimer) == INITIATE_TASK_TIMER)
      {
        xTimerStop(soft_timer_handle_3,10);
        xTaskCreate(initiate_task,"Alarm task",2048*2,NULL,4,initiate_taskHandle);
        //TWDT
        esp_task_wdt_reset();
        //esp_task_wdt_delete(NULL);
      }
    else if (pcTimerGetName(xTimer) == SHIFT_REBOOT_TIMER)
      {
        if (reboot_timer_flag == 1)
        {
          reboot_timer_flag++;
          xTimerStop(soft_timer_handle_2,10);
          xTimerChangePeriod(soft_timer_handle_2,pdMS_TO_TICKS(remain_time),10);
          xTimerStart(soft_timer_handle_2,10);
          ESP_LOGI(TAG,"Do day r nha %lld",remain_time);
          //TWDT
          //esp_task_wdt_reset();          
        }
        else if (reboot_timer_flag == 2)
        {
          xTimerStop(soft_timer_handle_2,10);
          ESP_LOGI("Restart","SHIFT_REBOOT_TIMER");
          restart_esp();
          //TWDT
          esp_task_wdt_reset();
          //esp_task_wdt_delete(NULL);
        }
        
      }
    else if (pcTimerGetName(xTimer) == STATUS_TIMER_1)
      {
        idle_trigger_1 = true;       
        xTimerStop(soft_timer_handle_5,10);
        char message_text_1[500];
        char message_mqtt_1[500];
        ESP_LOGI("IDLE","Idle status Unit 1");
        if (error_rtc == false)
        ds1307_get_time(&rtc_i2c, &idle_time);        
        
       sprintf(message_mqtt_1,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Idle,34,34,34,idle_time.tm_year, idle_time.tm_mon,idle_time.tm_mday, idle_time.tm_hour, idle_time.tm_min, idle_time.tm_sec,34); 
       esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_1,message_mqtt_1,0,1,1);         

        sprintf(message_text_1,"%04d-%02d-%02dT%02d:%02d:%02d,%d",idle_time.tm_year,idle_time.tm_mon,idle_time.tm_mday,idle_time.tm_hour,idle_time.tm_min, idle_time.tm_sec,Idle);
        write_to_sd(message_text_1,&CURRENT_STATUS_FILE);
        if(panic_stop_1 == false)
        xTimerStart(soft_timer_handle_5,10);       
      }
    else if (pcTimerGetName(xTimer) == RECONNECT_TIMER)
      {
        xTimerStop(soft_timer_handle_6,10);
        esp_wifi_connect(); 
        reconnect_time = 0;  
        //TWDT
        esp_task_wdt_reset(); 
        //esp_task_wdt_delete(NULL);    
      }
    else if (pcTimerGetName(xTimer) == BOOT_CONNECT_TIMER)
      {
        xTimerStop(soft_timer_handle_7,10);
        ESP_LOGI("Restart","BOOT_CONNECT_TIMER");
        esp_restart();
        //TWDT
        esp_task_wdt_reset();
        //esp_task_wdt_delete(NULL);
      }  
    else if (pcTimerGetName(xTimer) == STATUS_TIMER_2)
      {
        idle_trigger_2 = true;       
        xTimerStop(soft_timer_handle_8,10);
        char message_text_2[500];
        char message_mqtt_2[500];
        ESP_LOGI("IDLE","Idle status Unit 2");
        if (error_rtc == false)
        ds1307_get_time(&rtc_i2c, &idle_time);        
        
        sprintf(message_mqtt_2,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Idle,34,34,34,idle_time.tm_year, idle_time.tm_mon,idle_time.tm_mday, idle_time.tm_hour, idle_time.tm_min, idle_time.tm_sec,34); 
        esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_2,message_mqtt_2,0,1,1);         

        sprintf(message_text_2,"%04d-%02d-%02dT%02d:%02d:%02d,%d",idle_time.tm_year,idle_time.tm_mon,idle_time.tm_mday,idle_time.tm_hour,idle_time.tm_min, idle_time.tm_sec,Idle);
        write_to_sd(message_text_2,&CURRENT_STATUS_FILE);
        if(panic_stop_2 == false)
        xTimerStart(soft_timer_handle_8,10);
      }
}
/*----------------------------------------------------------------------*/
static bool IRAM_ATTR timer_group_isr_callback(void *args)
{
    BaseType_t high_task_awoken = pdFALSE;
    return high_task_awoken == pdTRUE; // return whether we need to yield at the end of ISR
}
/*----------------------------------------------------------------------*/
/**
 * @brief Initialize selected timer of timer group
 *
 * @param group Timer Group number, index from 0
 * @param timer timer ID, index from 0
 * @param auto_reload whether auto-reload on alarm event
 * @param timer_interval_sec interval of alarm
 */
static void timer_init_isr(int group, int timer, bool auto_reload, int timer_interval_sec)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_DIS,
        .auto_reload = auto_reload,
    }; // default clock source is APB
    timer_init(group, timer, &config);

    /* Timer's counter will initially start from value below.
       Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(group, timer, 0);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(group, timer, timer_interval_sec * TIMER_SCALE);
    timer_enable_intr(group, timer);

    timer_info_t *timer_info = calloc(1, sizeof(timer_info_t));
    timer_info->timer_group = group;
    timer_info->timer_idx = timer;
    timer_info->auto_reload = auto_reload;
    timer_info->alarm_interval = timer_interval_sec;
    timer_isr_callback_add(group, timer, timer_group_isr_callback, timer_info, 0);
    timer_start(group, timer);
}
/*----------------------------------------------------------------------*/
void mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{ //TWDT
  //esp_task_wdt_add(NULL);

  switch (event->event_id)
  {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    xTaskNotify(taskHandle, MQTT_CONNECTED, eSetValueWithOverwrite);
    xTimerStop(soft_timer_handle_1,10);
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    xTimerStart(soft_timer_handle_1,10);
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;
  case MQTT_EVENT_PUBLISHED:
    // ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
    xTaskNotify(taskHandle, MQTT_PUBLISHED, eSetValueWithOverwrite);
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    xQueueSend(mqtt_mess_events,&event,portMAX_DELAY);
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;  
  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    break;
  default:
    // TWDT
    esp_task_wdt_reset();
    //esp_task_wdt_delete(NULL);
    ESP_LOGI(TAG, "Other event id:%d", event->event_id);
    break;
  }
}
/*----------------------------------------------------------------------*/
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  mqtt_event_handler_cb(event_data);
}
/*----------------------------------------------------------------------*/
static void mqtt_mess_task(void *arg)
{
  esp_mqtt_event_handle_t mqtt_event;
  while (1)
  {
    //lockVariable();
    char message_text[500];
    if(xQueueReceive(mqtt_mess_events, &mqtt_event, 1000/portTICK_PERIOD_MS))
    { 
      if (mqtt_event->topic_len == 28)      // IMM/P010-left/CollectingData
      {
        my_json = cJSON_Parse(mqtt_event->data);
        cfg_info_1.timestamp = cJSON_GetObjectItem(my_json,"Timestamp")->valuestring;        
        cfg_info_1.command = cJSON_GetObjectItem(my_json,"Command")->valueint;
        cfg_info_1.is_configured = true;
        sprintf(message_text,"%s,%d",cfg_info_1.timestamp,cfg_info_1.command);
        if (cfg_info_1.command == 0)
          {
            DA_cmd_1 = 1;
            ESP_LOGI("DA command","Stop collecting data mould 1");            
          }
        else
          DA_cmd_1 = 0;
        //write_to_sd(message_text,CURRENT_STATUS_FILE);        
        ESP_LOGI("Collecting cmd","Mould 1: Timestamp:%s, Command collecting:%d",cfg_info_1.timestamp,cfg_info_1.command);
        // TWDT
        esp_task_wdt_reset();
      }
      else if (mqtt_event->topic_len == 29)      // IMM/P010-right/CollectingData
      {
        my_json = cJSON_Parse(mqtt_event->data);
        cfg_info_2.timestamp = cJSON_GetObjectItem(my_json,"Timestamp")->valuestring;        
        cfg_info_2.command = cJSON_GetObjectItem(my_json,"Command")->valueint;
        cfg_info_2.is_configured = true;
        sprintf(message_text,"%s,%d",cfg_info_2.timestamp,cfg_info_2.command);
        if (cfg_info_2.command == 0)
          {
            DA_cmd_2 = 1;
            ESP_LOGI("DA command","Stop collecting data mould 2");            
          }
        else
          DA_cmd_2 = 0;
        //write_to_sd(message_text,CURRENT_STATUS_FILE);  
        ESP_LOGI("Collecting cmd","Mould 2: Timestamp:%s, Command collecting:%d",cfg_info_2.timestamp,cfg_info_2.command);
        // TWDT
        esp_task_wdt_reset();
      }
    }
    else
    {
      // TWDT
      esp_task_wdt_reset();
    }
   // unlockVariable();
  }
}
/*----------------------------------------------------------------------*/
static void app_notifications(void *arg)
{ 
  uint32_t command = 0; 
  bool power_on = false;
  char message_text[500];
  char message_mqtt[500];   

  esp_mqtt_client_config_t mqttConfig = {
      .uri = CONFIG_BROKER_URL,
      .username = CONFIG_USER_NAME,
      .password = CONFIG_USER_PASSWORD,
      .disable_clean_session = false,
      .task_stack = 4096,
      .reconnect_timeout_ms = 30000,
      .lwt_topic = MACHINE_LWT_TOPIC,
      .lwt_msg = "{\"machines\": [{\"machineId\": \"P010-left\"},{\"machineId\": \"P010-right\"}]}",
      .lwt_qos = 0,
      .disable_auto_reconnect = false
      // .keepalive = 200
      };
    
  // Publish RTC fail message
  if ( ds1307_init_desc(&rtc_i2c, I2C_NUM_0, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO) != ESP_OK) 
    {      
      error_rtc = true; 
      // char mess_fb[200];
      // sprintf(mess_fb,"{%cMess%c:%d}",34,34,RTCfail);
      // if(esp_mqtt_client_publish(client,FEEDBACK_TOPIC,mess_fb,0,1,0) == -1)
      //   {
      //     esp_mqtt_client_enqueue(client,FEEDBACK_TOPIC,mess_fb,0,1,0,1);
      //   }  
      ESP_LOGE(pcTaskGetTaskName(0), "Could not init device descriptor.");      
    }
  else 
    {
      esp_err_t err = ds1307_get_time(&rtc_i2c, &local_time);
      if (err != ESP_OK)
        {
          ESP_LOGE(TAG,"RTC error");
          error_rtc = true;
          // char mess_fb[200];
          // sprintf(mess_fb,"{%cMess%c:%d}",34,34,RTCfail);
          // if(esp_mqtt_client_publish(client,FEEDBACK_TOPIC,mess_fb,0,1,0) == -1)
          //   {
          //     esp_mqtt_client_enqueue(client,FEEDBACK_TOPIC,mess_fb,0,1,0,1);
          //   } 
          //Cai dat thoi gian trong truong hop module RTC co van de
          local_time.tm_year = 2022;
          local_time.tm_mon  = 4;
          local_time.tm_mday = 20;
          local_time.tm_hour = 9;
          local_time.tm_min  = 0;
          local_time.tm_sec = 0;
          inject_time_1.tm_year = 2022;
          inject_time_1.tm_mon  = 4;
          inject_time_1.tm_mday = 20;
          inject_time_1.tm_hour = 9;
          inject_time_1.tm_min  = 0;
          inject_time_1.tm_sec = 0;
          inject_time_2.tm_year = 2022;
          inject_time_2.tm_mon  = 4;
          inject_time_2.tm_mday = 20;
          inject_time_2.tm_hour = 9;
          inject_time_2.tm_min  = 0;
          inject_time_2.tm_sec = 0;
          idle_time.tm_year = 2022;
          idle_time.tm_mon  = 4;
          idle_time.tm_mday = 20;
          idle_time.tm_hour = 9;
          idle_time.tm_min  = 0;
          idle_time.tm_sec = 0;
        }
        ESP_LOGI("TAG","Get time OK %04d-%02d-%02dT%02d:%02d:%02d",local_time.tm_year, local_time.tm_mon,local_time.tm_mday, local_time.tm_hour,local_time.tm_min, local_time.tm_sec);
    }    
    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
  while (1)
  { 
    //lockVariable();
    //ESP_LOGI("Tracking","App notification\n");
    xTaskNotifyWait(0, 0, &command, portMAX_DELAY);    
    switch (command)
    {
    case WIFI_CONNEECTED:      
      esp_mqtt_client_start(client);     
      break;

    case MQTT_CONNECTED:
      xTaskCreate(mqtt_mess_task,"MQTT mess",2048*2,NULL,9,NULL);      
      esp_mqtt_client_subscribe(client,CONFIGURATION_TOPIC_LEFT,1);
      esp_mqtt_client_subscribe(client,CONFIGURATION_TOPIC_RIGHT,1); 
      esp_mqtt_client_publish(client,MACHINE_LWT_TOPIC,"",0,1,1); 
      sprintf(message_mqtt,"[{%cname%c:%cmachineStatus%c,%cvalue%c:%d,%ctimestamp%c:%c%04d-%02d-%02dT%02d:%02d:%02d%c}]",34,34,34,34,34,34,Run,34,34,34,local_time.tm_year, local_time.tm_mon,local_time.tm_mday, local_time.tm_hour, local_time.tm_min, local_time.tm_sec,34); 
      esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_1,message_mqtt,0,1,1);
      esp_mqtt_client_publish(client,MACHINE_STATUS_TOPIC_2,message_mqtt,0,1,1);               
      break;

    case INITIATE_SETUP:     
      
      break;

    default:
       break;       
    }
    //unlockVariable();
    vTaskDelay(100);
  }
}
/*----------------------------------------------------------------------*/
static void wifi_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{   
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
      {
          esp_wifi_connect();
          xTaskNotify(taskHandle,SYSTEM_READY, eSetValueWithOverwrite);
          ESP_LOGI(TAG,"connecting...\n");
          //TWDT
          esp_task_wdt_reset();
          
      } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
      {
          ESP_LOGI("Wifi","disconnected\n");
          reconnect_time++;
          if (reconnect_time < CONFIG_ESP_MAXIMUM_RETRY)
            {
              esp_wifi_connect();
              // ESP_LOGI("Wifi","Event Wifi Start reconnect after 5 min");
              //TWDT
              esp_task_wdt_reset();
              
            }
          else if (reconnect_time >= CONFIG_ESP_MAXIMUM_RETRY)
            {
              if (boot_to_reconnect == false)
              {
                boot_to_reconnect = true;
                xTimerStart(soft_timer_handle_7,10);
                ESP_LOGI("Wifi","Boot after 1h");
                //TWDT
                esp_task_wdt_reset();
              }
              esp_mqtt_client_stop(client);
              xTimerStart(soft_timer_handle_6,10);
              //TWDT
              esp_task_wdt_reset();
              
            }
      } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
      {
          ESP_LOGI("Wifi","Got ip\n");
          boot_to_reconnect = false;
          xTimerStop(soft_timer_handle_6,10);
          xTimerStop(soft_timer_handle_7,10);
          
          xTaskNotify(taskHandle, WIFI_CONNEECTED, eSetValueWithOverwrite);
          //TWDT
          esp_task_wdt_reset();
         
      }  
    else
    {
      //TWDT
      esp_task_wdt_reset();
      
    }           
}
/*----------------------------------------------------------------------*/
void wifiInit()
{
       
    ESP_ERROR_CHECK(esp_netif_init()); // Lib inheritance    
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Lib inheritance
    
    esp_netif_t *my_sta = esp_netif_create_default_wifi_sta(); // Lib inheritance
    
    // Static IP address

    esp_netif_dhcpc_stop(my_sta);

    esp_netif_ip_info_t ip_info;

    IP4_ADDR(&ip_info.ip, 192, 168, 1, 199);     //192, 168, 1, 207
   	IP4_ADDR(&ip_info.gw, 192, 168, 1, 1);       //192, 168, 1, 207
    
   	IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_set_ip_info(my_sta, &ip_info);
    
    // Lib inheritance
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));     

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());
    // if wifi initializing OK, then feeding WDT
    esp_task_wdt_reset();    
}
void app_main()
{ 
  // NVS init
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
      // NVS partition was truncated and needs to be erased
      ESP_ERROR_CHECK(nvs_flash_erase());
      err = nvs_flash_init();
    }  
     
  error_sd_card = false;
  error_rtc = false;
  boot_to_reconnect = false;    
  idle_trigger_1 = false;
  idle_trigger_2 = false;
   
  // Set up Hardware timer mould 1 (phuc vu cho viec do thoi gian chu ky ep)
  timer_init_isr(TIMER_GROUP_0, TIMER_0, false,45000);
  timer_init_isr(TIMER_GROUP_0, TIMER_1, false,45000);
  // timer_pause(TIMER_GROUP_0, TIMER_0);
  // timer_pause(TIMER_GROUP_0, TIMER_1);
  // timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0x00000000ULL);
  // timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0x00000000ULL);
  // Set up Hardware timer mould 2 (phuc vu cho viec do thoi gian chu ky ep)
  timer_init_isr(TIMER_GROUP_1, TIMER_0, false,45000);
  timer_init_isr(TIMER_GROUP_1, TIMER_1, false,45000);
  // timer_pause(TIMER_GROUP_1, TIMER_0);
  // timer_pause(TIMER_GROUP_1, TIMER_1);
  // timer_set_counter_value(TIMER_GROUP_1, TIMER_0, 0x00000000ULL);
  // timer_set_counter_value(TIMER_GROUP_1, TIMER_1, 0x00000000ULL);
  
  // Set up Software timer (phuc vu cho viec nhu cac timer alarm)
  soft_timer_handle_1 = xTimerCreate(RECONNECT_BROKER_TIMER,pdMS_TO_TICKS(100000),false,(void *)1, &vSoftTimerCallback);  
  soft_timer_handle_3 = xTimerCreate(INITIATE_TASK_TIMER,pdMS_TO_TICKS(5000),false,(void *)3, &vSoftTimerCallback); // Timer Initiate
  soft_timer_handle_5 = xTimerCreate(STATUS_TIMER_1,pdMS_TO_TICKS(2000000),false,(void *)5, &vSoftTimerCallback); // Timer xac dinh trang thai Idle Unit 1
  soft_timer_handle_6 = xTimerCreate(RECONNECT_TIMER,pdMS_TO_TICKS(300000),false,(void *)6,&vSoftTimerCallback);// Timer scan wifi
  soft_timer_handle_7 = xTimerCreate(BOOT_CONNECT_TIMER,pdMS_TO_TICKS(3600000),false,(void *)7,&vSoftTimerCallback); //Timer Reboot to connect
  soft_timer_handle_8 = xTimerCreate(STATUS_TIMER_2,pdMS_TO_TICKS(2000000),false,(void *)8, &vSoftTimerCallback); // Timer xac dinh trang thai Idle Unit 2
  //soft_timer_handle_9 = xTimerCreate(NG1_TIMER,pdMS_TO_TICKS(500),false,(void *)9, &vSoftTimerCallback); // Timer xac dinh trang thai Idle Unit 2  
  xTimerStart(soft_timer_handle_5,10); 
  xTimerStart(soft_timer_handle_3,10);
  xTimerStart(soft_timer_handle_8,10);
     
  //Initializing the task watchdog subsystem with an interval of 2 seconds
  //esp_task_wdt_init(2, true);  

  // Ket noi Wifi
  wifiInit();
 
  // Khoi dong cac Task
  
  xTaskCreatePinnedToCore(app_notifications,"App logic",2048*4, NULL, 5, &taskHandle,0); //Core1
  /* Initialize GPIO*/
  //zero-initialize the config structure.
  gpio_config_t io_conf = {};
  //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //change gpio intrrupt type for PIN 36 & 39
    gpio_set_intr_type(GPIO_INPUT_CYCLE_1, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(GPIO_INPUT_CYCLE_2, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(GPIO_INPUT_BAD_1, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(GPIO_INPUT_BAD_2, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(GPIO_INPUT_BAD_3, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(GPIO_INPUT_BAD_4, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(GPIO_INPUT_SETUP_1, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(GPIO_INPUT_SETUP_2, GPIO_INTR_ANYEDGE);

    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));     
    xTaskCreatePinnedToCore(gpio_task_example, "gpio_task_example", 2048*4, NULL, 10, NULL,0); 

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_CYCLE_1, gpio_isr_handler, (void*) GPIO_INPUT_CYCLE_1);
        //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_CYCLE_2, gpio_isr_handler, (void*) GPIO_INPUT_CYCLE_2);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_BAD_1, gpio_isr_handler, (void*) GPIO_INPUT_BAD_1);
     //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_BAD_2, gpio_isr_handler, (void*) GPIO_INPUT_BAD_2);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_BAD_3, gpio_isr_handler, (void*) GPIO_INPUT_BAD_3);
     //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_BAD_4, gpio_isr_handler, (void*) GPIO_INPUT_BAD_4);
     //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_SETUP_1, gpio_isr_handler, (void*) GPIO_INPUT_SETUP_1);
     //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_SETUP_2, gpio_isr_handler, (void*) GPIO_INPUT_SETUP_2);
    
    // MQTT event handle queue
    mqtt_mess_events = xQueueCreate(10,sizeof(esp_mqtt_event_handle_t));
    //int cnt = 0;
    // while(1) {
    //     printf("cnt: %d\n", cnt++);
    //     vTaskDelay(1000 / portTICK_RATE_MS);
    //     // gpio_set_level(GPIO_OUTPUT_IO_0, cnt % 2);
    //     // gpio_set_level(GPIO_OUTPUT_IO_1, cnt % 2);
    // }
}

static void initiate_task(void* arg)
{
  if (error_rtc == false)
  {
    ds1307_get_time(&rtc_i2c, &local_time);
    ds1307_get_time(&rtc_i2c, &inject_time_1);
    ds1307_get_time(&rtc_i2c, &inject_time_2);
  }
  
  if ((local_time.tm_year < 2022) || (local_time.tm_year > 2035) || (local_time.tm_mon > 12) || (local_time.tm_mday > 31))
    {
      local_time.tm_year = 2022;
      local_time.tm_mon  = 4;
      local_time.tm_mday = 20;
      char mess_fb[200];      
      error_rtc = true;  // Bat co bao loi thoi gian
      ESP_LOGE("TAG","The time on RTC board is not exactly --> Set default day");
    }
  
  remain_time = 10000;
  int64_t sub_time = 40000000; //28800000
  struct tm previousday ;
  
  get_previous_day(&previousday,&local_time);
  
  ESP_LOGI(TAG,"previous %04d-%02d-%02d",previousday.tm_year,previousday.tm_mon,previousday.tm_mday);
  
  ESP_LOGI(TAG,"today %04d-%02d-%02d %02d:%02d:%02d",local_time.tm_year,local_time.tm_mon,local_time.tm_mday,local_time.tm_hour,local_time.tm_min,local_time.tm_sec);
  shift = 1;
  int ofset = 0;
  if (((local_time.tm_hour > 13)&&(local_time.tm_hour <=21)) || ((local_time.tm_hour == 13) && (local_time.tm_min >= 45)))
  {
    shift = 2;
    alarm_time.tm_hour = 21;
    alarm_time.tm_min = 45;
    alarm_time.tm_sec = 0;
    ESP_LOGI(TAG,"1");
  }
  else if ((local_time.tm_hour > 21) || ((local_time.tm_hour == 21) && (local_time.tm_min >= 45)))
  {
    shift = 3;
    alarm_time.tm_hour = 5;
    alarm_time.tm_min = 45;
    alarm_time.tm_sec = 0;
    ESP_LOGI(TAG,"1");
  }
  else if ((local_time.tm_hour < 5) || ((local_time.tm_hour == 5) && (local_time.tm_min <= 44)))
  { 
    shift = 3;
    ofset = 1;
    ESP_LOGI(TAG,"2");
    alarm_time.tm_hour = 5;
    alarm_time.tm_min = 45;
    alarm_time.tm_sec = 0;
  }
  else
  {
    alarm_time.tm_hour = 13;
    alarm_time.tm_min = 45;
    alarm_time.tm_sec = 0;
    ESP_LOGI(TAG,"3");
  }
  remain_time = calculate_diff_time(local_time,alarm_time);
  load_value_nvs(&cycle_id_1,"cycle_id_1");
  load_value_nvs(&cycle_id_2,"cycle_id_2");
  load_value_nvs(&NG1,"NG1");
  load_value_nvs(&NG2,"NG2");
  load_value_nvs(&NG3,"NG3");
  load_value_nvs(&NG4,"NG4");
  load_value_nvs(&total_operation_time_1,"total_operation_time_1");
  load_value_nvs(&total_operation_time_2,"total_operation_time_2");

  struct stat file_status_stat;
  struct stat file_cycle_stat;
  // Define file name to write
  if (ofset == 0)    //file name co ngay la ngay hom nay
  {
    sprintf(CURRENT_STATUS_FILE,"/sdcard/s%d%02d%02d%02d.csv",shift,local_time.tm_mday,local_time.tm_mon,local_time.tm_year%2000);
    if (stat(CURRENT_STATUS_FILE,&file_status_stat) != 0)
      write_to_sd("Timestamp,MachineStatus",CURRENT_STATUS_FILE);
  
    sprintf(CURRENT_CYCLE_FILE,"/sdcard/c%d%02d%02d%02d.csv",shift,local_time.tm_mday,local_time.tm_mon,local_time.tm_year%2000);
    if (stat(CURRENT_CYCLE_FILE,&file_cycle_stat) != 0)
      write_to_sd("injectionTime,injectionCycle,timestamp",CURRENT_CYCLE_FILE);
    // else // Neu nhu file do co roi chung to ca do da chay roi, nhu vay can load cycle_id len
    //   {
    //     load_value_nvs(&cycle_id_1,"cycle_id_1");
    //     load_value_nvs(&cycle_id_2,"cycle_id_2");
    //     load_value_nvs(&NG1,"NG1");
    //     load_value_nvs(&NG2,"NG2");
    //     load_value_nvs(&NG3,"NG3");
    //     load_value_nvs(&NG4,"NG4");
    //     load_value_nvs(&total_operation_time_1,"total_operation_time_1");
    //     load_value_nvs(&total_operation_time_2,"total_operation_time_2");
    //   }
  }
  else              //file name co ngay la ngay hom qua
  {
    sprintf(CURRENT_STATUS_FILE,"/sdcard/s%d%02d%02d%02d.csv",shift,previousday.tm_mday,previousday.tm_mon,previousday.tm_year%2000);
    if (stat(CURRENT_STATUS_FILE,&file_status_stat) != 0)
      write_to_sd("Timestamp,MachineStatus",CURRENT_STATUS_FILE);
  
    sprintf(CURRENT_CYCLE_FILE,"/sdcard/c%d%02d%02d%02d.csv",shift,previousday.tm_mday,previousday.tm_mon,previousday.tm_year%2000);
    if (stat(CURRENT_CYCLE_FILE,&file_cycle_stat) != 0)
      write_to_sd("injectionTime,injectionCycle,timestamp",CURRENT_CYCLE_FILE);
    // else  // Neu nhu file do co roi chung to ca do da chay roi, nhu vay can load cycle_id len
    //   {
    //     load_value_nvs(&cycle_id_1,"cycle_id_1");
    //     load_value_nvs(&cycle_id_2,"cycle_id_2");
    //     load_value_nvs(&NG1,"NG1");
    //     load_value_nvs(&NG2,"NG2");
    //     load_value_nvs(&NG3,"NG3");
    //     load_value_nvs(&NG4,"NG4");
    //     load_value_nvs(&total_operation_time_1,"total_operation_time_1");
    //     load_value_nvs(&total_operation_time_2,"total_operation_time_2");
    //   }
  }
 
  if(remain_time > 40000000)
  {
    remain_time = remain_time - sub_time;
    reboot_timer_flag = 1;
    soft_timer_handle_2 = xTimerCreate(SHIFT_REBOOT_TIMER,pdMS_TO_TICKS(40000000),false,(void *)2, &vSoftTimerCallback);
  }
  else
  {
    reboot_timer_flag = 2;
    soft_timer_handle_2 = xTimerCreate(SHIFT_REBOOT_TIMER,pdMS_TO_TICKS(remain_time),false,(void *)2, &vSoftTimerCallback);
  }

  ESP_LOGI("Remain time","time %lld",remain_time);
  xTimerStart(soft_timer_handle_2,10);

  // Set an alarm to restart esp32 
  xTaskNotify(taskHandle,INITIATE_SETUP, eSetValueWithOverwrite);
  // remain_time = remain_time - sub_time;
  // Delete task
  delete_task();
}


