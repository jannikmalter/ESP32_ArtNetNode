#define FIRMWARE_VERSION "1.0"

#include <stdio.h>
#include <stdint.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

#define STORAGE_NAMESPACE "storage"



// ETHERNET CONFIG
#define	PIN_PHY_POWER	16
#define MY_ETH_PHY_ADDR 1
#define MY_ETH_PHY_RST_GPIO -1
#define MY_ETH_MDC_GPIO 23
#define MY_ETH_MDIO_GPIO 18


//ARTNET CONFIG
#define BUFLEN 1024 //Max length of buffer
#define PORT 6454   //The port on which to listen for incoming data
#define TCP_PORT 1337

#define MAX_UNIS 10

#define MAX_LEDS 600

uint_fast8_t        NUM_UNIS = 10;
uint_fast16_t		active_leds = 100;
uint_fast16_t 		DMX_patch[MAX_UNIS] = {0,1,2,3,4,5,6,7,8,9};
uint_fast16_t		offsets[MAX_UNIS+1] = {0,10,20,30,40,50,60,70,80,90,100};

uint_fast8_t newFrame = 0;


const char* long_name 		= "Krach vom Fach LED Tube";
const char* short_name 		= "LED Tube";
const char* serial_number 	= "6";
const char* fw_version      = FIRMWARE_VERSION ;

static const char	*TAG = "eth_example";

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
}

void save_settings()
{
	nvs_handle my_handle;
	nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);

	nvs_set_u16(my_handle, "ACTIVE_LEDS", (uint16_t)active_leds);
	nvs_set_u16(my_handle, "NUM_UNIS", (uint16_t)NUM_UNIS);

	nvs_set_blob(my_handle, "DMXPATCH", DMX_patch, MAX_UNIS * sizeof(uint_fast16_t));
	nvs_set_blob(my_handle, "OFFSETS", offsets, (MAX_UNIS+1) * sizeof(uint_fast16_t));
	
	nvs_commit(my_handle);
	nvs_close(my_handle);
	
	printf("settings saved\n");
}

void load_settings()
{
	uint16_t active_leds_load;
	uint16_t NUM_UNIS_load;

	nvs_handle my_handle;	
	nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);	

	if( nvs_get_u16(my_handle, "ACTIVE_LEDS", &active_leds_load) == ESP_OK &&
		nvs_get_u16(my_handle, "NUM_UNIS"   , &NUM_UNIS_load)    == ESP_OK)
	{
		NUM_UNIS = (uint_fast8_t)NUM_UNIS_load;
		active_leds = (uint_fast16_t)active_leds_load;		
		
		size_t required_size = MAX_UNIS * sizeof(uint_fast16_t);
		nvs_get_blob(my_handle, "DMXPATCH", DMX_patch, &required_size);

		required_size = (MAX_UNIS+1) * sizeof(uint_fast16_t);
		nvs_get_blob(my_handle, "OFFSETS", offsets, &required_size);

		printf("settings loaded\n");
	}
	else
	{
		printf("no settings found\n");
		save_settings();
	}

	nvs_close(my_handle);

	
}

void removeCRLF(char* str, int len)
{
	uint8_t c;
	uint_fast8_t i = 0;
	
	if (str != NULL)
	{
		c = str[i];
		while ((c != 0) && (i < len))
		{
			if (c == 10 || c == 13)
			{
				str[i] = 0;				
			}
			i++;
			c = str[i];
		}
	}
}

int get_state(char *replyBuf, int len)
{
	int n = 0;
	
	memset(replyBuf, 0, len);

	n =     sprintf(replyBuf + n, "\tACTIVE_LEDS: %u\r\n", offsets[NUM_UNIS]);
    n = n + sprintf(replyBuf + n, "\r\n");
	n = n + sprintf(replyBuf + n, "\tNUM_UNIS: %u\r\n", NUM_UNIS);	
	n = n + sprintf(replyBuf + n, "\r\n");
	n = n + sprintf(replyBuf + n, "\tOFFSETS\tPATCH\r\n");
	
	for (uint_fast8_t i = 0; i < NUM_UNIS; i++)	
		n = n + sprintf(replyBuf + n, "\t%u:\t%u\r\n", offsets[i], DMX_patch[i]);
	n = n + sprintf(replyBuf + n, "\r\n");	
	return n;
}

void tcp_task()
{
	 
	printf("TCP Task started\n"); 
	 
	int port = TCP_PORT;

	uint_fast16_t n;

	int server_fd, client_fd, err;
	struct sockaddr_in server, client;

	char *inputBuf;
	char *replyBuf;

	inputBuf = (char*) calloc(BUFLEN, sizeof(char));
	replyBuf = (char*) calloc(BUFLEN, sizeof(char));

	while (1)
	{
		printf("init socket\n"); 
		
		vTaskDelay(1000 / portTICK_PERIOD_MS);   //remove?

		server_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (server_fd < 0)
		{
			printf("could not create socket\n");
			fflush(NULL);
			break;
		}

		server.sin_family = AF_INET;
		server.sin_port = htons(port);
		server.sin_addr.s_addr = htonl(INADDR_ANY);

		int opt_val = 1;
		setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof opt_val);

		err = bind(server_fd, (struct sockaddr *)&server, sizeof(server));
		if (err < 0)
		{
			printf("bind error\n");
			fflush(NULL);
			break;
		}

		err = listen(server_fd, 128);
		if (err < 0)
		{
			printf("listen error\n");
			fflush(NULL);
			break;
		}
		
		printf("socket created\n"); 
		

		while (1)
		{
			
			printf("waiting for connection\n"); 
			
			
			socklen_t client_len = sizeof(client);
			client_fd = accept(server_fd, (struct sockaddr *)&client, &client_len);

			if (client_fd < 0)
				break;	

			vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
			
			printf("connection established\n");
			
			memset(replyBuf, 0, BUFLEN);
			n = sprintf(replyBuf, "%s %s\r\nFW %s\r\n\r\n", long_name, serial_number, fw_version);
			send(client_fd, replyBuf, n, 0);

			n = get_state(replyBuf, BUFLEN);
			send(client_fd, replyBuf, n, 0);
			
			
			
			while (1)
			{
				printf("waiting for message\n");
				
				memset(inputBuf, 0, BUFLEN);
				int read = recv(client_fd, inputBuf, BUFLEN, 0);
				inputBuf[BUFLEN - 1] = 0;
				
				printf("message received\n");

				if (!read)
					break;

				if (read <= 0)
					break;
				
				removeCRLF(inputBuf, read);
				
				if (inputBuf[0] != 0)
				{
									
					char *token;
					token = strtok(inputBuf, " ");

					printf("1");

					if (strcmp(token, "PATCH") == 0)
					{
						printf("1");
						uint_fast8_t i = 0;
						token = strtok(NULL, " ");
						printf("2"); 
						while (token && i < NUM_UNIS)
						{
							printf("3");
							DMX_patch[i] = atoi(token);
							printf("4");
							token = strtok(NULL, " ");
							printf("5");
							i++;
						}
						printf("6");

						save_settings();
					}	

					else if (strcmp(token, "OFFSETS") == 0)
					{
						uint_fast8_t i = 0;
						uint_fast16_t o = 0;
						uint_fast16_t o0 = 0;

						token = strtok(NULL, " "); 
						while (token != NULL && i < NUM_UNIS)
						{
							o = atoi(token);
							if (o>o0 && o<active_leds) offsets[i] = o;
							o0 = offsets[i];
							token = strtok(NULL, " ");
							i++;
						}
						save_settings();
					}								
					
					
					else if (strcmp(token, "ACTIVE_LEDS") == 0)
					{
						token = strtok(NULL, " ");
						uint_fast16_t input = atoi(token);
						if (input > offsets[NUM_UNIS-1] && input <= MAX_LEDS)
						{
							active_leds = input;
							offsets[NUM_UNIS] = active_leds;							
							save_settings();
							esp_restart();
						}
					}
					else if (strcmp(token, "NUM_UNIS") == 0)
					{
						token = strtok(NULL, " ");
						uint_fast8_t input = atoi(token);
						if (input <= 10)
						{
							NUM_UNIS = input;
							offsets[NUM_UNIS] = active_leds;
							save_settings();
							esp_restart();
						}
					}
					

				}
				
				n = get_state(replyBuf, BUFLEN);
				send(client_fd, replyBuf, n, 0);
				
			}
			close(client_fd);
			vTaskPrioritySet(NULL, tskIDLE_PRIORITY);
			printf("client disconnected\n");
		}
		close(server_fd);
		printf("socket removed\n");
	}
}

void eth_task()
{
	const char artnet_header[8] = "Art-Net";

	uint32_t opcode = 0;
	uint32_t DMXlength = 0;
	uint32_t portaddress = 0;
	int32_t bufaddress = 0;


	uint_fast16_t cpy_len = 0;
	uint_fast16_t start_led = 0;

	char *ArtNetBuf;
	char *replyBuf;

	struct sockaddr_in si_me, si_other;
	uint32_t s, slen = sizeof(si_other), recv_len;

	ArtNetBuf = (char*) calloc(BUFLEN, sizeof(char));
	replyBuf = (char*) calloc(BUFLEN, sizeof(char));

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	memset((char *)&si_me, 0, sizeof(si_me));
	si_me.sin_family = AF_INET;
	si_me.sin_port = htons(PORT);
	si_me.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(s, (struct sockaddr *)&si_me, sizeof(si_me));

	while (1)
	{
		recv_len = recvfrom(s, ArtNetBuf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen);
		if (recv_len >= 10)
		{
			if (strcmp(ArtNetBuf, artnet_header) == 0)
			{
				opcode = (ArtNetBuf[9] << 8) | ArtNetBuf[8];
				if (opcode == 0x5000)
				{
					portaddress = (ArtNetBuf[15] << 8) | ArtNetBuf[14];
					bufaddress = -1;
					for (uint_fast8_t i = 0; i < NUM_UNIS; i++)
					{
						if (DMX_patch[i] == portaddress)
						{
							bufaddress = i;
							break;
						}
					}
					if (bufaddress != -1)
					{
						DMXlength = (ArtNetBuf[16] << 8) | ArtNetBuf[17];
						start_led = offsets[bufaddress];

						cpy_len = offsets[bufaddress+1] - start_led;	
						if (cpy_len > DMXlength / 3) cpy_len = DMXlength;

 						for(int i = 0; i < cpy_len; i++){};
							
						if (portaddress == DMX_patch[0]){};
														
					}
				}
				else if (opcode == 0x2000 || opcode == 0x6000 || opcode == 0x7000)
				{
					memset(replyBuf, 0, BUFLEN);
					memcpy(replyBuf, artnet_header, 8);
					replyBuf[8] = 0x00;
					replyBuf[9] = 0x21;

					memcpy(replyBuf + 10, &si_me.sin_addr.s_addr, 4);

					replyBuf[14] = 0x36;
					replyBuf[15] = 0x19;

					replyBuf[23] = 1 << 7 | 1 << 6 | 1 << 5;

					sprintf(replyBuf + 26 , "%s %s", short_name, serial_number);
					sprintf(replyBuf + 44 , "%s %s", long_name, serial_number);

					sendto(s, replyBuf, 239, 0, (struct sockaddr *)&si_other, slen);
				}
			}
		}
	}
}

void app_main() {
	
	
	// Initialize TCP/IP network interface (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());
	
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

	// Create ETH netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

	// --- Ethernet MAC + PHY (ESP32 EMAC + LAN8720 over RMII) ---
    // Power the PHY (WT32-ETH01 uses a clock-enable / power pin)
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_PHY_POWER,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level((gpio_num_t)PIN_PHY_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(10));


	// Common MAC/PHY configs
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr         = MY_ETH_PHY_ADDR;       // usually 0/1 on WT32-ETH01
    phy_cfg.reset_gpio_num   = MY_ETH_PHY_RST_GPIO;   // -1 if not wired

	// ESP32-specific MAC config: SMI pins and RMII ref clock
    eth_esp32_emac_config_t esp32_emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_cfg.smi_gpio.mdc_num  = MY_ETH_MDC_GPIO;   // WT32-ETH01: 23
    esp32_emac_cfg.smi_gpio.mdio_num = MY_ETH_MDIO_GPIO;  // WT32-ETH01: 18
    esp32_emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    // WT32-ETH01 provides external 50 MHz REF_CLK to GPIO0:
    esp32_emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_cfg.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO; // GPIO0


   // Create MAC/PHY instances
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

    // Install driver and attach to netif
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Register your app handlers (system handlers are auto-registered in v5.x)
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &got_ip_event_handler, NULL));

    // Start Ethernet state machine
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    // ------------------------------------------------------------

	
	
	// Initialize NVS
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		// NVS partition was truncated and needs to be erased
		// Retry nvs_flash_init
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	




	load_settings();

	xTaskCreatePinnedToCore((TaskFunction_t)eth_task, "eth_task", 2048, NULL, (tskIDLE_PRIORITY), NULL, 1);
    xTaskCreatePinnedToCore((TaskFunction_t)tcp_task, "tcp_task", 2048, NULL, (tskIDLE_PRIORITY+1), NULL, 1);
}