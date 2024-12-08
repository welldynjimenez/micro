/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

//Librerías utilizadas por el ejemplo MQTT
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

//Librerías llamadas por el usuario
#include "driver/gpio.h"
#include <stdlib.h>
#include "freertos/timers.h"


//Etiquetas creadas para informar al usuario de ciertos eventos a través de la terminal
static const char *TAG = "mqtt_example";


//Macros a utilizar para facilitar el entendimiento del código
#define ESTADO_1 1
#define ESTADO_2 2
#define ESTADO_3 3
#define ESTADO_4 4
#define ESTADO_5 5
#define DELAY_2 500
#define DELAY_3 100
#define DELAY_4 1000
#define INCREMENTO_DELAY_5 250

//Macros utilizadas para manejar el tamaño de memoria que se le asignan a las tareas
#define STACK_SIZE 1024*4

//Macros a utilizar para los GPIOs del ESP32
#define LED 2


// Variables para la interacción recepción/envío con el broker MQTT
esp_mqtt_client_handle_t client; // Creación del cliente que se conectará con el broker MQTT
int msg_id;                      // Variable para manejar las funciones de los mensajes a enviar
bool conexion_mqtt = false;      // Variable para verificar la conexión MQTT


//Variables utilizadas para la creación de ñas colas que intercambiarán datos entre las diferentes tareas
QueueHandle_t xQueue1 = 0;  //Declaración de la cola de mensajes que se utilizará para enviar datos desde la tarea de entrada hacia la tarea del led
QueueHandle_t xQueue2 = 0;  //Declaración de la cola de mensajes que se utilizará para enviar datos desde la tarea de entrada hacia la tarea de salida


// Variables que usaremos para crear el timer que se ejecutará cada cierto tiempo
TimerHandle_t xTimers;      // Variable para crear el timer
int periodo_del_timer = 50; // Periodo de tiempo del timer que se creará
int timerID = 1;            // Identificación única del timer que se usará


// Variables para interactuar con la máquina de estado
bool boton_mqtt = false; // Botón para interactuar desde el MQTT con la máquina de estado
int DELAY_5 = 100;       // Valor inicial del delay del estado 5 de la máquina de estado


// Variables para interactuar con los parpadeos del led
bool led_estado_2 = false; // Variable para almacenar el valor lógico del led durante el parpadeo cuando está en el estado 2
bool led_estado_3 = false; // Variable para almacenar el valor lógico del led durante el parpadeo cuando está en el estado 3
bool led_estado_4 = false; // Variable para almacenar el valor lógico del led durante el parpadeo cuando está en el estado 4
bool led_estado_5 = false; // Variable para almacenar el valor lógico del led durante el parpadeo cuando está en el estado 5
int cambio_de_estado = 0;  // Variable registrar cuando haya un cambio de estado y modificar el parpadeo del led
int tiempo = 0;            // Variable para registrar el tiempo que ha transcurrido mediante las interrupciones del timer
int flanco = 0;            // Variable para registrar la cantidad de flancos (o cambios de estados) del led durante su parpadeo para el estado 5


//Inicializamos todos los estados temporales en el estado de apagado
int ESTADO_ANTERIOR = ESTADO_1;
int ESTADO_ACTUAL = ESTADO_1;
int ESTADO_SIGUIENTE = ESTADO_1;


//Prototipos de la funciones que se utilizarán en la máquina de estados
int Funcion_Estado_1(void);
int Funcion_Estado_2(void);
int Funcion_Estado_3(void);
int Funcion_Estado_4(void);
int Funcion_Estado_5(void);


//Prototipos de las tareas que se utilizarán
void Tarea_entrada( void * pvParameters );
void Tarea_led( void * pvParameters );
void Tarea_salida( void * pvParameters );


//Función para cambiar el estado del led mediante la interrupción del timer
void Modificacion_del_delay(TimerHandle_t pxTimer)
{
    //Cuando cambiamos de estado actual reiniciamos los diferentes estados del led a 0
    if (cambio_de_estado != ESTADO_ACTUAL)     
    {
        led_estado_2 = false;
        led_estado_3 = false;
        led_estado_4 = false;
        led_estado_5 = false;
        DELAY_5 = 100;
        tiempo = 0;
        cambio_de_estado = ESTADO_ACTUAL;
    }

    //En el estado actual 2 el estado del led cambiará cada 500 milisegundos, implementamos un contador para que sume 50ms cada vez que el timer ejecute una interrupción
    if (ESTADO_ACTUAL == ESTADO_2)
    {
        if (tiempo < DELAY_2)
        {
            tiempo += periodo_del_timer;
        }
        else
        {
            led_estado_2 = !led_estado_2;
            tiempo = 0;
        } 
    }

    //En el estado actual 3 el estado del led cambiará cada 100 milisegundos, implementamos un contador para que sume 50ms cada vez que el timer ejecute una interrupción
    else if (ESTADO_ACTUAL == ESTADO_3)
    {
        if (tiempo < DELAY_3)
        {
            tiempo += periodo_del_timer;
        }
        else
        {
            led_estado_3 = !led_estado_3;
            tiempo = 0;
        } 
    }

    //En el estado actual 4 el estado del led cambiará cada 1 segundo, implementamos un contador para que sume 50ms cada vez que el timer ejecute una interrupción
    else if (ESTADO_ACTUAL == ESTADO_4)
    {
        if (tiempo < DELAY_4)
        {
            tiempo += periodo_del_timer;
        }
        else
        {
            led_estado_4 = !led_estado_4;
            tiempo = 0;
        } 
    }

    //En el estado actual 5 el estado del led cambiará inicialmente cada 100ms e irá incrementando en 250ms su parpadeo hasta llegar a 1s
    //Cada 6 flancos del estado del led se incrementará 250ms la duración de su parpadeo, implmentamos un contador para que sume 50ms cada vez que el timer ejecute una interrupción
    else if (ESTADO_ACTUAL == ESTADO_5)
    {
        if (tiempo < DELAY_5)
        {
            tiempo += periodo_del_timer;
        }
        else
        {
            //Se acumulan la cantidad de flancos y si excede la cantidad establecida pues se incrementa el delay del parpadeo del estado 5 y se resetea la cantidad de flancos contados
            if (flanco < 10)
            {
                ++flanco;
            }
            else
            {
                //Cada vez que se supere el número de flancos establecidos se incrementa el delay del parpadeo del estado 5 en 250ms y si el delay excede 1s pues se resetea nuevamente a 100ms
                if (DELAY_5 < 1000)
                {
                    DELAY_5 += INCREMENTO_DELAY_5;
                }
                else
                {
                    DELAY_5 = 100;
                }
                
                flanco = 0;
            }
            
            led_estado_5 = !led_estado_5;
            tiempo = 0;
        } 
    }
}


//Función para crear el timer
esp_err_t Crear_Timer(void)
{
    // Aquí creamos el timer que utilizaremos para incrementar el delay del estado 5
    xTimers = xTimerCreate("Timer",                            // Just a text name, not used by the kernel.
                           (pdMS_TO_TICKS(periodo_del_timer)), // The timer period in ticks.
                           pdTRUE,                             // The timers will auto-reload themselves when they expire.
                           (void *)timerID,                    // Assign each timer a unique id equal to its array index.
                           Modificacion_del_delay                // Each timer calls the same callback when it expires.
    );

    if (xTimers == NULL)
    {
        // The timer was not created.
    }
    else
    {
        // Start the timer.  No block time is specified, and even if one was
        // it would be ignored because the scheduler has not yet been
        // started.
        if (xTimerStart(xTimers, 0) != pdPASS)
        {
            // The timer could not be set into the Active state.
        }
    }

    return ESP_OK;
}


//Función para crear las tareas que usaremos constantemente
esp_err_t Crear_tareas(void)
{
    //Creamos las siguientes variables para utilizarlas en la creación de las tareas
    static uint8_t ucParameterToPass;
    TaskHandle_t xHandle = NULL;

    //Creamos la tarea para las entradas con una prioridad de 1
    xTaskCreate(Tarea_entrada,
                "Tarea para las entradas",
                STACK_SIZE,
                &ucParameterToPass,
                1,
                &xHandle);

    //Creamos la tarea para las entradas con una prioridad de 2
    xTaskCreate(Tarea_led,
                "Tarea para el control del led",
                STACK_SIZE,
                &ucParameterToPass,
                2,
                &xHandle);

    //Creamos la tarea para las entradas con una prioridad de 3
    xTaskCreate(Tarea_salida,
                "Tarea para las salidas",
                STACK_SIZE,
                &ucParameterToPass,
                3,
                &xHandle);

    return ESP_OK;
}


static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    //int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        //Cuando nos conectamos con el bróker MQTT nos subscribimos a los diferentes tópicos con los que trabajaremos
        msg_id = esp_mqtt_client_subscribe(client, "Control del led/Indicador de estado", 0);
        //ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", " ", 0, 0, 1);
        //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "Control del led/Boton de control", 0);
        //ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_publish(client, "Control del led/Boton de control", "0", 0, 0, 0);
        //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "Control del led/Led", 0);
        //ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
        //ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        conexion_mqtt = true;   //Confirmamos la conexión MQTT

        /*
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        */

        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

        conexion_mqtt = false;  //Confirmamos la desconexión MQTT
        
        break;

    case MQTT_EVENT_SUBSCRIBED:
        //ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);

        /*
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        */

        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        //ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        
        /*
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        */

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        char topico_MQTT [100];                                 //Creamos la variable que nos almcenará el tópico en el que se ha publicado
        strncpy(topico_MQTT, event->topic, event->topic_len);   //Copiamos el nombre del tópico en la variable que creamos anteriormente
        topico_MQTT [event->topic_len] = '\0';                  //Aseguramos de que la cadena de texto copiada esté terminada en '\0'

        char dato_MQTT [100];                                   //Creamos la variable que nos almcenará el dato recibido por MQTT
        strncpy(dato_MQTT, event->data, event->data_len);       //Copiamos el dato recibido por MQTT en la variable que creamos anteriormente
        dato_MQTT [event->data_len] = '\0';                     //Aseguramos de que la cadena de texto copiada esté terminada en '\0'

        //Comparamos el nombre del topico en el que se publicó con el nombre del tópico que nos interesa y el dato recibido con el número 1
        //Si son iguales entonces se ejecuta la sentencia if
        if((strcmp(topico_MQTT, "Control del led/Boton de control")) == (strcmp(dato_MQTT, "1")))
        {   
            //Actualizamos la variable de control para abrir/cerrar el porton
            boton_mqtt = true;
            strcpy(dato_MQTT, "0");
        }

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());


    //Configuramos el pin 2 del ESP32 como salida
    gpio_reset_pin(LED);
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);

    //Creación de las colas que comunicarán las diferentes tareas del programa
    xQueue1 = xQueueCreate(10, sizeof(uint32_t));
    xQueue2 = xQueueCreate(10, sizeof(uint32_t));

    //LLamamos a la función que creará el timer
    Crear_Timer();

    //LLamamos a la función que creará las tareas
    Crear_tareas();

    //Llamamos a esta función para conectarnos al broker MQTT
    mqtt_app_start();
}


void Tarea_entrada( void * pvParameters )
{
    //Máquina de estado
    for( ;; )
    {
        //Estado apagado
        if (ESTADO_SIGUIENTE == ESTADO_1)
        {
            ESTADO_SIGUIENTE = Funcion_Estado_1();
        }

        //Estado de parpadeo 500ms
        if (ESTADO_SIGUIENTE == ESTADO_2)
        {
            ESTADO_SIGUIENTE = Funcion_Estado_2();
        }

        //Estado de parpadeo 100ms
        if (ESTADO_SIGUIENTE == ESTADO_3)
        {
            ESTADO_SIGUIENTE = Funcion_Estado_3();
        }

        //Estado de parpadeo 1s
        if (ESTADO_SIGUIENTE == ESTADO_4)
        {
            ESTADO_SIGUIENTE = Funcion_Estado_4();
        }

        //Estado de parpadeo 0.1-1s
        if (ESTADO_SIGUIENTE == ESTADO_5)
        {
            ESTADO_SIGUIENTE = Funcion_Estado_5();
        }
    }
}


void Tarea_led( void * pvParameters )
{
    //Variable local para almacenar el estado actual de la máquina de estado
    int estado_actual_local = 0;
    
    for( ;; )
    {
        //Recepción de los datos de otras tareas utilizando lass colas
        xQueueReceive(xQueue1, &estado_actual_local, pdMS_TO_TICKS(10));

        //Sentencia switch case para seleccionar el parapadeo del led en base al estaado actual de la máquina de estado
        switch (estado_actual_local)
        {
            //Estado 1: en este estado el led permanece apagado
            case ESTADO_1:
                gpio_set_level(LED, false);
                vTaskDelay(pdMS_TO_TICKS(10));
                msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
                break;

            //Estado 2: en este estado el led parpadea cada 500 milisegundos
            case ESTADO_2:
                gpio_set_level(LED, led_estado_2);

                if (led_estado_2 == true)
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "ON", 0, 0, 1);
                }
                else
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
                }
                break;

            //Estado 3: en este estado el led parpadea cada 100 milisegundos
            case ESTADO_3:
                gpio_set_level(LED, led_estado_3);
                
                if (led_estado_3 == true)
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "ON", 0, 0, 1);
                }
                else
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
                }
                break;

            //Estado 4: en este estado el led parpadea cada 1 segundo
            case ESTADO_4:
                gpio_set_level(LED, led_estado_4);
                
                if (led_estado_4 == true)
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "ON", 0, 0, 1);
                }
                else
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
                }
                break;

            //Estado 5: en este estado el led hace un barrido de parpadeo desde 0.1 segundo hasta 1 segundo con un incremento de 250 milisegundos cada 5 segundos
            case ESTADO_5:
                gpio_set_level(LED, led_estado_5);
                
                if (led_estado_5 == true)
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "ON", 0, 0, 1);
                }
                else
                {
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Led", "OFF", 0, 0, 1);
                }
                break;

            default:
                break;
        }
    }
}


void Tarea_salida( void * pvParameters )
{
    //Variables para guardar e imprimir el estado actual de la máquina de estado por la serial y por MQTT
    bool informar_por_serial = false;
    bool informar_por_mqtt = false;
    int estado_momentaneo = 0;
    int estado_actual_local = 0;

    for( ;; )
    {
        //Recepción de los datos de otras tareas utilizando lass colas
        xQueueReceive(xQueue2, &estado_actual_local, pdMS_TO_TICKS(10));

        //Reseteo de las variables de impresión de información al detectar el cambio de estado de la máquina de estados
        if (estado_momentaneo != estado_actual_local)     
        {
            informar_por_serial = false;
            informar_por_mqtt = false;
            estado_momentaneo = estado_actual_local;
        }
        
        //Sentencia switch case para seleccionar el parapadeo del led en base al estaado actual de la máquina de estado
        switch (estado_actual_local)
        {
            //Informa una única vez al usuario por la serial y por MQTT que el led se encuentra en el estado 1
            case ESTADO_1:
                vTaskDelay(pdMS_TO_TICKS(10));

                //Confirmamos que el mensaje no se haya enviado anteriormente para solo mandarlo una vez por la serial
                if (informar_por_serial == false)
                {
                    //Enviamos el estado actual en el que se encuentra el led por la serial
                    printf("\nESTADO ACTUAL: ESTADO 1 - APAGADO\n");
                    informar_por_serial = true;
                }

                //Confirmamos la conexión MQTT antes de enviar el mensaje al usuario
                if ((conexion_mqtt == true) && (informar_por_mqtt == false))
                {
                    //Enviamos el estado actual en el que se encuentra el led al celular por MQTT
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", "ESTADO 1 - APAGADO", 0, 0, 1);
                    informar_por_mqtt = true;
                }
                break;

            //Informa una única vez al usuario por la serial y por MQTT que el led se encuentra en el estado 2
            case ESTADO_2:
                vTaskDelay(pdMS_TO_TICKS(10));

                //Confirmamos que el mensaje no se haya enviado anteriormente para solo mandarlo una vez por la serial
                if (informar_por_serial == false)
                {
                    //Enviamos el estado actual en el que se encuentra el led por la serial
                    printf("\nESTADO ACTUAL: ESTADO 2 - 500ms\n");
                    informar_por_serial = true;
                }

                //Confirmamos la conexión MQTT antes de enviar el mensaje al usuario
                if ((conexion_mqtt == true) && (informar_por_mqtt == false))
                {
                    //Enviamos el estado actual en el que se encuentra el led al celular por MQTT
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", "ESTADO 2 - 500ms", 0, 0, 1);
                    informar_por_mqtt = true;
                }
                break;

            //Informa una única vez al usuario por la serial y por MQTT que el led se encuentra en el estado 3
            case ESTADO_3:
                vTaskDelay(pdMS_TO_TICKS(10));

                //Confirmamos que el mensaje no se haya enviado anteriormente para solo mandarlo una vez por la serial
                if (informar_por_serial == false)
                {
                    //Enviamos el estado actual en el que se encuentra el led por la serial
                    printf("\nESTADO ACTUAL: ESTADO 3 - 100ms\n");
                    informar_por_serial = true;
                }

                //Confirmamos la conexión MQTT antes de enviar el mensaje al usuario
                if ((conexion_mqtt == true) && (informar_por_mqtt == false))
                {
                    //Enviamos el estado actual en el que se encuentra el led al celular por MQTT
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", "ESTADO 3 - 100ms", 0, 0, 1);
                    informar_por_mqtt = true;
                }
                break;

            //Informa una única vez al usuario por la serial y por MQTT que el led se encuentra en el estado 4
            case ESTADO_4:
                vTaskDelay(pdMS_TO_TICKS(10));

                //Confirmamos que el mensaje no se haya enviado anteriormente para solo mandarlo una vez por la serial
                if (informar_por_serial == false)
                {
                    //Enviamos el estado actual en el que se encuentra el led por la serial
                    printf("\nESTADO ACTUAL: ESTADO 4 - 1s\n");
                    informar_por_serial = true;
                }

                //Confirmamos la conexión MQTT antes de enviar el mensaje al usuario
                if ((conexion_mqtt == true) && (informar_por_mqtt == false))
                {
                    //Enviamos el estado actual en el que se encuentra el led al celular por MQTT
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", "ESTADO 4 - 1s", 0, 0, 1);
                    informar_por_mqtt = true;
                }
                break;

            //Informa una única vez al usuario por la serial y por MQTT que el led se encuentra en el estado 5
            case ESTADO_5:
                vTaskDelay(pdMS_TO_TICKS(10));

                //Confirmamos que el mensaje no se haya enviado anteriormente para solo mandarlo una vez por la serial
                if (informar_por_serial == false)
                {
                    //Enviamos el estado actual en el que se encuentra el led por la serial
                    printf("\nESTADO ACTUAL: ESTADO 5 - 0.1-1s\n");
                    informar_por_serial = true;
                }

                //Confirmamos la conexión MQTT antes de enviar el mensaje al usuario
                if ((conexion_mqtt == true) && (informar_por_mqtt == false))
                {
                    //Enviamos el estado actual en el que se encuentra el led al celular por MQTT
                    msg_id = esp_mqtt_client_publish(client, "Control del led/Indicador de estado", "ESTADO 5 - 0.1-1s", 0, 0, 1);
                    informar_por_mqtt = true;
                }
                break;

            default:
                break;
        }
    }
}


int Funcion_Estado_1(void)
{
    //Actualización de los estados
    ESTADO_ANTERIOR = ESTADO_ACTUAL;
    ESTADO_ACTUAL = ESTADO_1;

    //Envío de datos a otras tareas usando las diferentes colas
    xQueueSend(xQueue1, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    xQueueSend(xQueue2, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));

    //Actualización de los datos
    boton_mqtt = false;

    //Loop infinito
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        //Estado 1 ------>> Estado 2
        if (boton_mqtt == true)
        {
            return ESTADO_2;
        }
    }
}


int Funcion_Estado_2(void)
{
    //Actualización de los estados
    ESTADO_ANTERIOR = ESTADO_ACTUAL;
    ESTADO_ACTUAL = ESTADO_2;

    //Envío de datos a otras tareas usando las diferentes colas
    xQueueSend(xQueue1, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    xQueueSend(xQueue2, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));

    //Actualización de los datos
    boton_mqtt = false;

    //Loop infinito
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        //Estado 2 ------>> Estado 3
        if (boton_mqtt == true)
        {
            return ESTADO_3;
        }
    }
}


int Funcion_Estado_3(void)
{
    //Actualización de los estados
    ESTADO_ANTERIOR = ESTADO_ACTUAL;
    ESTADO_ACTUAL = ESTADO_3;

    //Envío de datos a otras tareas usando las diferentes colas
    xQueueSend(xQueue1, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    xQueueSend(xQueue2, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    
    //Actualización de los datos
    boton_mqtt = false;

    //Loop infinito
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        //Estado 3 ------>> Estado 4
        if (boton_mqtt == true)
        {
            return ESTADO_4;
        }
    } 
}


int Funcion_Estado_4(void)
{
    //Actualización de los estados
    ESTADO_ANTERIOR = ESTADO_ACTUAL;
    ESTADO_ACTUAL = ESTADO_4;

    //Envío de datos a otras tareas usando las diferentes colas
    xQueueSend(xQueue1, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    xQueueSend(xQueue2, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));

    //Actualización de los datos
    boton_mqtt = false;

    //Loop infinito
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        //Estado 4 ------>> Estado 5
        if (boton_mqtt == true)
        {
            return ESTADO_5;
        }
    }
}


int Funcion_Estado_5(void)
{
    //Actualización de los estados
    ESTADO_ANTERIOR = ESTADO_ACTUAL;
    ESTADO_ACTUAL = ESTADO_5;

    //Envío de datos a otras tareas usando las diferentes colas
    xQueueSend(xQueue1, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));
    xQueueSend(xQueue2, &ESTADO_ACTUAL, pdMS_TO_TICKS(100));

    //Actualización de los datos
    boton_mqtt = false;

    //Loop infinito
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));

        //Estado 5 ------>> Estado 1
        if (boton_mqtt == true)
        {
            return ESTADO_1;
        }
    }
}