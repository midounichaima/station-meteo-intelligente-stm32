#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>

// ==================== CONFIGURATION WIFI / MQTT ====================
// IMPORTANT : ne mettez jamais vos vrais identifiants ici si ce fichier
// est public. Utilisez un fichier "config.h" local (ignoré par git)
// ou des variables d'environnement lors du build.
#define WIFI_SSID        "VOTRE_SSID_WIFI"
#define WIFI_PASSWORD    "VOTRE_MOT_DE_PASSE_WIFI"
#define MQTT_BROKER_IP   "IP_DU_BROKER_MQTT"      // ex: "192.168.1.100"
#define MQTT_PORT        "1883"
#define MQTT_CLIENT_ID   "stm32_water"
#define MQTT_TOPIC_EAU   "pfa/capteurs/niveau_eau"
#define MQTT_TOPIC_PLUIE "pfa/capteurs/pluie"
#define MQTT_TOPIC_TEMP  "pfa/capteurs/temperature"
#define MQTT_TOPIC_SOL   "pfa/capteurs/humidite_sol"
#define THINGSPEAK_API_KEY  "VOTRE_CLE_API_THINGSPEAK"

// ==================== VARIABLES GLOBALES ====================
volatile uint32_t tick_count = 0;
volatile uint8_t  adc_ready  = 0;

// PA0=eau, PA1=pluie, PA3=sol
uint16_t adc_values[3];

// DS1621 — température entière signée (identique au doc source)
int8_t ds1621_temperature = 0;

char    tx_buffer[250];
uint8_t esp32_ready = 0;

// ==================== PROTOTYPES ====================
void    Delay_ms(uint32_t ms);
void    Delay_us(uint32_t us);
void    SysTick_Handler(void);

void    USART3_Init(void);
void    SendChar_USART3(char c);
void    SendString_USART3(const char *str);

void    ADC_DMA_Init(void);
void    DMA2_Stream0_IRQHandler(void);

// I2C + DS1621 (repris du doc source tel quel)
void    config_I2C1(void);
void    I2C_write_DS1621(uint8_t data);
void    I2C_read_DS1621(void);

void    ESP32_SendAT(const char *cmd);
void    ESP32_WaitMs(uint32_t ms);
uint8_t ESP32_Init(void);
uint8_t ESP32_MQTT_Connect(void);
void    ESP32_MQTT_Publish(void);
void    ThingSpeak_Publish(void);

// ==================== SYSTICK ====================
void SysTick_Handler(void) {
    tick_count++;
}

void Delay_ms(uint32_t ms) {
    static uint8_t initialized = 0;
    if (!initialized) {
        SystemCoreClockUpdate();
        SysTick_Config(SystemCoreClock / 1000);
        initialized = 1;
    }
    uint32_t start = tick_count;
    while ((tick_count - start) < ms);
}

void Delay_us(uint32_t us) {
    us *= (SystemCoreClock / 1000000) / 4;
    while (us--);
}

// ==================== USART3 (PB10 TX, PB11 RX) ====================
void USART3_Init(void) {
    RCC->AHB1ENR  |= (1 << 1);
    GPIOB->MODER  |= (2 << 20) | (2 << 22);
    GPIOB->AFR[1] |= (7 <<  8) | (7 << 12);
    RCC->APB1ENR  |= (1 << 18);
    USART3->BRR    = 139;
    USART3->CR1   |= (1 << 2) | (1 << 3) | (1 << 13);
}

void SendChar_USART3(char c) {
    while (!(USART3->SR & (1 << 7)));
    USART3->DR = c;
}

void SendString_USART3(const char *str) {
    while (*str) SendChar_USART3(*str++);
}

// ==================== I2C1 + DS1621 (repris du doc source) ====================
// PB6 = SCL, PB7 = SDA — 16 MHz APB1 — 100 kHz
// Adresse DS1621 : 0x90 (write) / 0x91 (read) — A2=A1=A0=GND

void config_I2C1(void) {
    RCC->AHB1ENR |= (1 << 1);               // Horloge GPIOB
    RCC->APB1ENR |= (1 << 21);              // Horloge I2C1
    GPIOB->MODER |= (2 << 12) | (2 << 14);  // PB6, PB7 en mode AF
    GPIOB->OTYPER |= (1 << 6) | (1 << 7);   // Open-drain
    GPIOB->AFR[0] |= (4 << 24) | (4 << 28); // AF4 = I2C1
    I2C1->CR1 = (1 << 15);                  // Reset I2C1
    I2C1->CR1 = 0;                          // Clear CR1 après reset
    I2C1->CR2 = 16;                         // FREQ = 16 MHz
    I2C1->CCR = 80;                         // 100 kHz (16 MHz / (2×80))
    I2C1->TRISE = 17;                       // Temps de montée (16+1)
    I2C1->CR1 |= (1 << 0);                  // Enable I2C1
}

// Envoi d'une commande/donnée vers DS1621
void I2C_write_DS1621(uint8_t data) {
    I2C1->CR1 |= (1 << 8);                  // START
    while(!(I2C1->SR1 & (1 << 0)));         // Attente SB
    I2C1->DR = 0x90;                        // Adresse DS1621 + Write
    while(!(I2C1->SR1 & (1 << 1)));         // Attente ADDR
    I2C1->SR2;                              // Clear ADDR
    I2C1->DR = data;                        // Envoi data
    while(!(I2C1->SR1 & (1 << 7)));         // Attente TxE
    I2C1->CR1 |= (1 << 9);                  // STOP
}

// Lecture température DS1621 (1 octet — résolution 1°C)
void I2C_read_DS1621(void) {
    I2C1->CR1 |= (1 << 8);                  // START
    while(!(I2C1->SR1 & (1 << 0)));         // Attente SB
    I2C1->DR = 0x90;                        // Adresse + Write
    while(!(I2C1->SR1 & (1 << 1)));         // Attente ADDR
    I2C1->SR2;                              // Clear ADDR
    I2C1->DR = 0xAA;                        // Commande Read Temperature
    while(!(I2C1->SR1 & (1 << 7)));         // Attente TxE

    I2C1->CR1 |= (1 << 8);                  // RESTART
    while(!(I2C1->SR1 & (1 << 0)));         // Attente SB
    I2C1->DR = 0x91;                        // Adresse + Read
    while(!(I2C1->SR1 & (1 << 1)));         // Attente ADDR
    I2C1->CR1 &= ~(1 << 10);                // NACK (dernier octet)
    I2C1->SR2;                              // Clear ADDR
    I2C1->CR1 |= (1 << 9);                  // STOP
    while(!(I2C1->SR1 & (1 << 6)));         // Attente RxNE
    ds1621_temperature = (int8_t)I2C1->DR;  // Lecture température
}

// ==================== ADC + DMA (PA0, PA1, PA3) ====================
void ADC_DMA_Init(void) {
    RCC->AHB1ENR |= 0x1;
    GPIOA->MODER |= (0x3 << 0)
                  | (0x3 << 2)
                  | (0x3 << 6);

    RCC->APB2ENR |= (1 << 8);
    ADC1->CR1    |= (1 << 8);
    ADC1->SQR1    = (2 << 20);
    ADC1->SQR3    = (0 << 0)
                  | (1 << 5)
                  | (3 << 10);
    ADC1->CR2    |= (1 << 8) | (1 << 9);
    ADC1->CR2    |= (1 << 0);

    RCC->AHB1ENR      |= (1 << 22);
    DMA2_Stream0->PAR  = (uint32_t)&ADC1->DR;
    DMA2_Stream0->M0AR = (uint32_t)adc_values;
    DMA2_Stream0->NDTR = 3;
    DMA2_Stream0->CR  |= (1 << 4)
                       | (1 << 8)
                       | (1 << 10)
                       | (1 << 11)
                       | (1 << 13);
    DMA2_Stream0->CR  |= (1 << 0);

    NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    ADC1->CR2 |= (1 << 30);
}

void DMA2_Stream0_IRQHandler(void) {
    if (DMA2->LISR & (1 << 5)) {
        DMA2->LIFCR |= (1 << 5);
        adc_ready = 1;
        ADC1->CR2 |= (1 << 0);
        ADC1->CR2 |= (1 << 30);
    }
}

// ==================== ESP32 AT ====================
void ESP32_SendAT(const char *cmd) {
    SendString_USART3(cmd);
    SendString_USART3("\r\n");
}

void ESP32_WaitMs(uint32_t ms) {
    Delay_ms(ms);
}

uint8_t ESP32_Init(void) {
    ESP32_SendAT("AT");
    ESP32_WaitMs(500);
    ESP32_SendAT("AT+RST");
    ESP32_WaitMs(3000);
    ESP32_SendAT("AT+CWMODE=1");
    ESP32_WaitMs(500);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASSWORD);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(8000);
    return 1;
}

uint8_t ESP32_MQTT_Connect(void) {
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"",
             MQTT_CLIENT_ID);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(1000);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTCONN=0,\"%s\",%s,1",
             MQTT_BROKER_IP, MQTT_PORT);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(3000);
    return 1;
}

void ESP32_MQTT_Publish(void) {
    char payload[10];

    // --- Niveau d'eau (PA0) ---
    uint8_t water_pct = (uint8_t)((adc_values[0] * 100UL) / 4095);
    snprintf(payload, sizeof(payload), "%d", water_pct);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", MQTT_TOPIC_EAU, payload);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(200);

    // --- Pluie (PA1) ---
    uint8_t rain_pct = 100 - (uint8_t)((adc_values[1] * 100UL) / 4095);
    snprintf(payload, sizeof(payload), "%d", rain_pct);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", MQTT_TOPIC_PLUIE, payload);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(200);

    // --- Humidité sol (PA3) ---
    uint8_t sol_pct = 100 - (uint8_t)((adc_values[2] * 100UL) / 4095);
    snprintf(payload, sizeof(payload), "%d", sol_pct);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", MQTT_TOPIC_SOL, payload);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(200);

    // --- Température DS1621 (entier signé, °C) ---
    snprintf(payload, sizeof(payload), "%d", (int)ds1621_temperature);
    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", MQTT_TOPIC_TEMP, payload);
    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(200);
}

void ThingSpeak_Publish(void) {
    uint8_t water_pct = (uint8_t)((adc_values[0] * 100UL) / 4095);
    uint8_t rain_pct  = 100 - (uint8_t)((adc_values[1] * 100UL) / 4095);
    uint8_t sol_pct   = 100 - (uint8_t)((adc_values[2] * 100UL) / 4095);

    snprintf(tx_buffer, sizeof(tx_buffer),
             "AT+HTTPCLIENT=2,0,\"http://api.thingspeak.com/update"
             "?api_key=%s"
             "&field1=%d"
             "&field2=%d"
             "&field3=%d"
             "&field4=%d\",,,1",
             THINGSPEAK_API_KEY,
             water_pct, rain_pct, sol_pct,
             (int)ds1621_temperature);

    ESP32_SendAT(tx_buffer);
    ESP32_WaitMs(2000);
}

// ==================== MAIN ====================
int main(void) {
    Delay_ms(1000);
    USART3_Init();

    // --- Initialisation I2C1 + DS1621 (séquence du doc source) ---
    config_I2C1();
    I2C_write_DS1621(0xAC);   // Access Config register
    I2C_write_DS1621(0x00);   // Mode continu (1SHOT=0)
    I2C_write_DS1621(0xEE);   // Start Convert
    Delay_ms(750);            // Attendre première conversion

    ADC_DMA_Init();
    ESP32_Init();
    ESP32_MQTT_Connect();
    esp32_ready = 1;

    uint32_t last_temp_time       = 0;
    uint32_t last_thingspeak_time = 0;

    while (1) {
        // DS1621 toutes les 2 secondes
        if ((tick_count - last_temp_time) >= 2000) {
            I2C_read_DS1621();
            last_temp_time = tick_count;
        }

        // Publier MQTT quand ADC prêt
        if (adc_ready && esp32_ready) {
            ESP32_MQTT_Publish();
            adc_ready = 0;
        }

        // ThingSpeak toutes les 16 secondes
        if ((tick_count - last_thingspeak_time) >= 16000) {
            ThingSpeak_Publish();
            last_thingspeak_time = tick_count;
        }

        Delay_ms(10);
    }
}
