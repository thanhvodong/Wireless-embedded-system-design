/**************DEFAULT CONFIGURATION*****************/
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
/****************************************************/
/****************CUSTOM INCLUDE**********************/
// Fan control includes
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "esp_timer.h"
// Get time settings
#include <time.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include <stdbool.h>
// DHT11 sensor includes
#include "dht11.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
/****************************************************/

/****************CUSTOM DEFINES**********************/
// MQRTT Broker URL
#define MQTT_BROKER_URL "mqtts://aadd81fb96c3478d9cf64cf7a57edd8e.s1.eu.hivemq.cloud"
// Fan configuration
#define ENA_PIN GPIO_NUM_14
#define IN1_PIN GPIO_NUM_27
#define IN2_PIN GPIO_NUM_26
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CH0 LEDC_CHANNEL_0
// DHT11 configuration
#define DHT11_PIN GPIO_NUM_4
#define MQTT_sizeoff 200
/****************************************************/

/*****************MACROS*****************************/
// SNTP Debug
static const char *TAG1 = "get_time";
char Current_Date_Time[100];
// MQTT Credentials
static const char *TAG = "mqtts_example";
static const char username[] = "CE232";
static const char password[] = "Thanh0512";
esp_mqtt_client_handle_t client;
// FAN Topic
char *fan_topic = "smarthome/fan/control";
char *fan_time_topic = "smarthome/fan/time";
int speed = 0;
static const char *TAG2 = "fan_control";
static bool is_publish_message = false;
bool fan_state = false;
esp_timer_handle_t fan_timer;
// DHT11 Topic
char *dht_topic = "smarthome/sensor";
unsigned long LastSendMQTT = 0;
char MQTT_BUFFER[MQTT_sizeoff];
char Str_ND[MQTT_sizeoff];
char Str_DA[MQTT_sizeoff];
char str[MQTT_sizeoff];
bool mqtt_connect_status = false;
/****************************************************/

/*************GET REAL TIME FUNCTIONS****************/
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG1, "Notification of a time synchronization event");
}
void get_current_time(char *date_time, int *hour, int *minute, int *second)
{
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    setenv("TZ", "UTC-07:00", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    // ESP_LOGI(TAG1, "The current date/time is: %s", strftime_buf);
    strcpy(date_time, strftime_buf);
    *hour = timeinfo.tm_hour;
    *minute = timeinfo.tm_min;
    *second = timeinfo.tm_sec;
}
static void sntp_setup(void)
{
    ESP_LOGI(TAG1, "Initalizing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_get_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    sntp_init();
}
static void obtain_time(void)
{
    sntp_setup();
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG1, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}
void set_systemtime_sntp()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2016 - 1900))
    {
        ESP_LOGI(TAG1, "Time not set yet. Connecting to WiFi and getting an NTP time to set time.");
        obtain_time();
        time(&now);
    }
}
/****************************************************/

/*************FAN FUNCTIONS**************************/
void fan_control(int speed)
{
    esp_rom_gpio_pad_select_gpio(ENA_PIN);
    esp_rom_gpio_pad_select_gpio(IN1_PIN);
    esp_rom_gpio_pad_select_gpio(IN2_PIN);
    gpio_set_direction(ENA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(IN1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(IN2_PIN, GPIO_MODE_OUTPUT);

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 1000,
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER};
    ledc_timer_config(&ledc_timer);
    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CH0,
        .duty = 0,
        .gpio_num = ENA_PIN,
        .speed_mode = LEDC_MODE,
        .timer_sel = LEDC_TIMER};
    ledc_channel_config(&ledc_channel);

    uint32_t duty = (speed * 255) / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CH0, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH0);
    gpio_set_level(IN1_PIN, 1);
    gpio_set_level(IN2_PIN, 0);
}
void fan_off_callback(void *arg)
{
    fan_control(0);
    fan_state = false;
    ESP_LOGI(TAG2, "Fan turned off.");
}
void start_fan_timer(int64_t duration_us)
{
    if (fan_timer != NULL)
    {
        esp_timer_stop(fan_timer);
        esp_timer_delete(fan_timer);
    }
    const esp_timer_create_args_t timer_args = {
        .callback = &fan_off_callback,
        .name = "fan_timer"};
    esp_timer_create(&timer_args, &fan_timer);
    esp_timer_start_once(fan_timer, duration_us);
}
void check_time_and_turn_on_fan(int start_hour, int start_minute, int start_second, int duration_hour, int duration_minute, int duration_second, int fan_speed)
{
    int current_hour, current_minute, current_second;
    get_current_time(Current_Date_Time, &current_hour, &current_minute, &current_second);
    if (start_hour == current_hour && start_minute == current_minute && start_second == current_second && !fan_state)
    {
        fan_control(fan_speed);
        fan_state = true;
        int64_t duration_us = ((int64_t)duration_hour * 3600 + (int64_t)duration_minute * 60 + (int64_t)duration_second) * 1000000;
        start_fan_timer(duration_us);
    }
}
// void monitor_and_control_fan(int start_hour, int start_minute, int start_second, int duration_hour, int duration_minute, int duration_second, int fan_speed)
// {
//     while (!fan_state) 
//     {
//         check_time_and_turn_on_fan(start_hour, start_minute, start_second, duration_hour, duration_minute, duration_second, fan_speed);
//         usleep(1000000);
//     }
// }
void monitor_and_control_fan_task(void *params)
{
    int *args = (int *)params;
    int start_hour = args[0];
    int start_minute = args[1];
    int start_second = args[2];
    int duration_hour = args[3];
    int duration_minute = args[4];
    int duration_second = args[5];
    int fan_speed = args[6];

    while (1)
    {
        check_time_and_turn_on_fan(start_hour, start_minute, start_second, duration_hour, duration_minute, duration_second, fan_speed);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}
void start_fan_monitoring(int start_hour, int start_minute, int start_second, int duration_hour, int duration_minute, int duration_second, int fan_speed)
{
    int *params = malloc(7 * sizeof(int));
    params[0] = start_hour;
    params[1] = start_minute;
    params[2] = start_second;
    params[3] = duration_hour;
    params[4] = duration_minute;
    params[5] = duration_second;
    params[6] = fan_speed;
    xTaskCreate(monitor_and_control_fan_task, "monitor_and_control_fan_task", 4096, params, 5, NULL);
}
void publish_fan_speed_message(int speed)
{
    char fan_speed_msg[10];
    snprintf(fan_speed_msg, sizeof(fan_speed_msg), "%d", speed);
    char publish_msg[50];
    snprintf(publish_msg, sizeof(publish_msg), "{\"Speed\":\"%s\"}", fan_speed_msg);
    int msg_id = esp_mqtt_client_publish(client, fan_topic, publish_msg, 0, 0, 0);
    if (msg_id != -1)
    {
        ESP_LOGI(TAG2, "Published fan speed message to MQTT: %s", fan_speed_msg);
        is_publish_message = true; 
    }
    else
    {
        ESP_LOGE(TAG2, "Failed to publish fan speed message to MQTT");
    }
}
/****************************************************/

/**************MQTT HANDLE DATAS*********************/
static void app_handle_mqtt_data(esp_mqtt_event_handle_t event)
{
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    printf("DATA Length= %d\r\n", event->data_len);

    char *topic = event->topic;
    char *data = event->data;

    if (strncmp(topic, fan_topic, strlen(fan_topic)) == 0)
    {
        data[event->data_len] = 0;
        if (strncmp(data, "{\"Speed\":\"", strlen("{\"Speed\":\"")) == 0)
        {
            int speed = atoi(data + strlen("{\"Speed\":\""));
            fan_control(speed);
        }
        else
        {
            int speed = atoi(data);
            fan_control(speed);
            publish_fan_speed_message(speed);
        }
    }
    else if (strncmp(topic, fan_time_topic, strlen(fan_time_topic)) == 0)
    {
        data[event->data_len] = 0;
        int start_hour, start_minute, start_second, duration_hour, duration_minute, duration_second, fan_speed;
        sscanf(data, "%d:%d:%d %d:%d:%d %d", &start_hour, &start_minute, &start_second, &duration_hour, &duration_minute, &duration_second, &fan_speed);
        start_fan_monitoring(start_hour, start_minute, start_second, duration_hour, duration_minute, duration_second, fan_speed);
    }
}
/****************************************************/

/**************DHT11 FUNCTIONS***********************/
void MQTT_DataJson(void)
{
    for (int i = 0; i < MQTT_sizeoff; i++)
    {
        MQTT_BUFFER[i] = 0;
        Str_ND[i] = 0;
        Str_DA[i] = 0;
    }
    sprintf(Str_ND, "%d", DHT11_read().temperature);
    sprintf(Str_DA, "%d", DHT11_read().humidity);
    strcat(MQTT_BUFFER, "{\"Temperature\":\"");
    strcat(MQTT_BUFFER, Str_ND);
    strcat(MQTT_BUFFER, "\",");
    strcat(MQTT_BUFFER, "\"Humidity\":\"");
    strcat(MQTT_BUFFER, Str_DA);
    strcat(MQTT_BUFFER, "\"}");
}
void delay(uint32_t time)
{
    vTaskDelay(time / portTICK_PERIOD_MS);
}
// void dht11_task(){
//     LastSendMQTT = esp_timer_get_time() / 1000;
//     while (1)
//     {
//         if (esp_timer_get_time() / 1000 - LastSendMQTT >= 1500)
//         {
//             if (mqtt_connect_status )
//             {
//                 MQTT_DataJson();
//                 esp_mqtt_client_publish(mqtt_client, dht_topic, MQTT_BUFFER, 0, 1, 0);
//                 ESP_LOGI(TAG2, "SEND MQTT %s", MQTT_BUFFER);
//                 delay(1000);
//                 taskYIELD();
//             }
//             LastSendMQTT = esp_timer_get_time() / 1000;
//         }
//         delay(50);
//     }
//     vTaskDelete(NULL);
// }
void read_sensor_task(void *params)
{
    while (1)
    {
        DHT11_init(DHT11_PIN);
        // int temperature = DHT11_read().temperature;
        // int humidity = DHT11_read().humidity;
        // ESP_LOGI(TAG, "Temperature: %d C, Humidity: %d %%", temperature, humidity);
        // snprintf(MQTT_BUFFER, MQTT_sizeoff, "{\"temperature\": %d, \"humidity\": %d}", temperature, humidity);
        MQTT_DataJson();
        esp_mqtt_client_publish(client, dht_topic, MQTT_BUFFER, 0, 1, 0);
        ESP_LOGI(TAG2, "SEND MQTT %s", MQTT_BUFFER);
        vTaskDelay(10000 / portTICK_PERIOD_MS); // Delay for 1 minute
    }
    vTaskDelete(NULL);
}
/****************************************************/
#if CONFIG_BROKER_CERTIFICATE_OVERRIDDEN == 1
static const uint8_t mqtt_eclipseprojects_io_pem_start[] = "-----BEGIN CERTIFICATE-----\n" CONFIG_BROKER_CERTIFICATE_OVERRIDE "\n-----END CERTIFICATE-----";
#else
extern const uint8_t mqtt_eclipseprojects_io_pem_start[] asm("_binary_mqtt_eclipseprojects_io_pem_start");
#endif
extern const uint8_t mqtt_eclipseprojects_io_pem_end[] asm("_binary_mqtt_eclipseprojects_io_pem_end");

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, fan_topic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, fan_time_topic, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        app_handle_mqtt_data(event);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        }
        else
        {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_BROKER_URL,
            .verification.certificate = (const char *)mqtt_eclipseprojects_io_pem_start
            },
        .credentials = {
            .username = username,
            .authentication.password = password
        },
    };
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    // LastSendMQTT = esp_timer_get_time() / 1000;
    // while (1)
    // {
    //     if (esp_timer_get_time() / 1000 - LastSendMQTT >= 1500)
    //     {
    //         if (mqtt_connect_status )
    //         {
    //             MQTT_DataJson();
    //             esp_mqtt_client_publish(mqtt_client, dht_topic, MQTT_BUFFER, 0, 1, 0);
    //             ESP_LOGI(TAG2, "SEND MQTT %s", MQTT_BUFFER);
    //             delay(1000);
    //             taskYIELD();
    //         }
    //         LastSendMQTT = esp_timer_get_time() / 1000;
    //     }
    //     delay(50);
    // }
    // vTaskDelete(NULL);
}
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    DHT11_init(DHT11_PIN);
    ESP_ERROR_CHECK(example_connect());
    set_systemtime_sntp();
    mqtt_app_start();
    xTaskCreate(read_sensor_task, "task", 4096, NULL, 5, NULL);
}
