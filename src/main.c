/*
 * ESP32 ArtNetNode  —  Art-Net -> DMX512 (7 outputs, software bit-bang)
 *
 * The DMX engine, Art-Net/UDP handling, TCP config interface and NVS logic are
 * ported VERBATIM from the original working OLIMEX ESP32-POE firmware
 * (ESP-IDF ~v4.0). Only the Ethernet bring-up has been modernized to the
 * ESP-IDF v5 API and parameterized to build for two boards, and one v4->v5 GPIO
 * enum name was fixed (GPIO_PIN_INTR_DISABLE -> GPIO_INTR_DISABLE in dmx_task).
 *
 * Build configuration is selected at compile time via -D flags in
 * platformio.ini — see the BUILD CONFIGURATION block below.
 */

#define FIRMWARE_VERSION "1.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "soc/dport_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"

#include "lwip/sockets.h"

/* ===========================================================================
 *  BUILD CONFIGURATION   (set via platformio.ini build_flags)
 *
 *  Board — selects the Ethernet PHY wiring. Define exactly one:
 *      BOARD_OLIMEX_ESP32_POE   Olimex ESP32-POE  (the deployed hardware)
 *      BOARD_WT32_ETH01         WT32-ETH01        (secondary target)
 *
 *  Variant — selects the DMX output GPIO order. Define exactly one:
 *      VARIANT_RACK             "rack" device
 *      VARIANT_MINI             "mini" device
 *
 *  "rack" and "mini" are the two physically built devices. They share the same
 *  board but wire the seven DMX outputs to different GPIOs / in a different
 *  order — see GPIO_patch below.
 * =========================================================================== */

#if defined(BOARD_OLIMEX_ESP32_POE)
	#define BOARD_NAME          "Olimex ESP32-POE"
	#define ETH_PHY_ADDR        0
	#define PIN_PHY_POWER       12
	#define ETH_PHY_RST_GPIO    -1
	#define ETH_MDC_GPIO        23
	#define ETH_MDIO_GPIO       18
	/* Olimex generates the 50 MHz RMII clock internally and drives it OUT on GPIO17. */
	#define ETH_RMII_CLK_MODE   EMAC_CLK_OUT
	#define ETH_RMII_CLK_GPIO   EMAC_CLK_OUT_180_GPIO   /* GPIO17 (RMII clock OUT, 180deg) */
#elif defined(BOARD_WT32_ETH01)
	#define BOARD_NAME          "WT32-ETH01"
	#define ETH_PHY_ADDR        1
	#define PIN_PHY_POWER       16
	#define ETH_PHY_RST_GPIO    -1
	#define ETH_MDC_GPIO        23
	#define ETH_MDIO_GPIO       18
	/* WT32 takes an external 50 MHz oscillator IN on GPIO0. */
	#define ETH_RMII_CLK_MODE   EMAC_CLK_EXT_IN
	#define ETH_RMII_CLK_GPIO   EMAC_CLK_IN_GPIO            /* GPIO0 */
#else
	#error "No board defined: set -DBOARD_OLIMEX_ESP32_POE or -DBOARD_WT32_ETH01 in platformio.ini build_flags."
#endif

#define NUM_OUT 7

/* DMX output GPIOs — one per physical output, in device-specific order.
 *   rack and mini are the two built devices; they differ in output order.
 * Constraint: every pin must be <= 31, because the bit-bang writes the
 * GPIO_OUT_W1TS/W1TC registers (0x3ff44008 / 0x3ff4400c), which only cover
 * GPIO0..31. */
#if defined(BOARD_OLIMEX_ESP32_POE)
	#if defined(VARIANT_RACK)
		#define VARIANT_NAME "rack"
		const char GPIO_patch[NUM_OUT] = {16, 15, 14, 13, 5, 4, 2};   /* Olimex "rack" */
	#elif defined(VARIANT_MINI)
		#define VARIANT_NAME "mini"
		const char GPIO_patch[NUM_OUT] = {5, 2, 4, 0, 16, 14, 15};    /* Olimex "mini" */
	#else
		#error "No variant defined: set -DVARIANT_RACK or -DVARIANT_MINI in platformio.ini build_flags."
	#endif
#elif defined(BOARD_WT32_ETH01)
	/* NOTE: on WT32-ETH01, GPIO16 = PHY power and GPIO0 = RMII clock, so the
	 * Olimex pin maps cannot be reused. No WT32 device has been built yet — the
	 * maps below are PLACEHOLDERS (GPIO16->17, GPIO0->12, both free on WT32).
	 * Verify against the actual WT32 wiring before relying on them. */
	#if defined(VARIANT_RACK)
		#define VARIANT_NAME "rack"
		const char GPIO_patch[NUM_OUT] = {17, 15, 14, 13, 5, 4, 2};   /* WT32 "rack" (TBD) */
	#elif defined(VARIANT_MINI)
		#define VARIANT_NAME "mini"
		const char GPIO_patch[NUM_OUT] = {5, 2, 4, 12, 17, 14, 15};   /* WT32 "mini" (TBD) */
	#else
		#error "No variant defined: set -DVARIANT_RACK or -DVARIANT_MINI in platformio.ini build_flags."
	#endif
#endif

#define STORAGE_NAMESPACE "storage"

/* NODE CONFIG */
#define BUFLEN 1024   /* Max length of buffer */
#define PORT 6454     /* The port on which to listen for incoming Art-Net */
#define TCP_PORT 1337

#define BREAK 30
#define MAB 10

static const char *TAG = "ArtNetNode";

uint8_t *DMXbuf;

uint_fast16_t DMX_patch[NUM_OUT] = {0, 0, 0, 0, 0, 0, 0};
uint_fast8_t DMX_repatch[NUM_OUT] = {0, 0, 0, 0, 0, 0, 0};

/* Cross-task reconfiguration handshake (no locks):
 *   stopFlag   - set by tcp_task (via stopDMX), polled by dmx_task & eth_task.
 *   dmxStopped - set by dmx_task once it has left vPortExitCritical and is
 *                idling; lets stopDMX *observe* the critical-section exit
 *                instead of assuming a fixed delay was long enough.
 * Both are written by one task and read by another, so they must be volatile
 * or the compiler may cache them in a register and never see the change. */
volatile uint_fast8_t stopFlag = 0;
volatile uint_fast8_t dmxStopped = 0;

uint_fast8_t synchronize = 0;
uint_fast16_t sync_addr = 0;
uint_fast16_t num_chan = 0;
volatile uint_fast8_t trigger = 0;

float refresh_rate;

/* Device IPv4 address in network byte order, captured from
 * IP_EVENT_ETH_GOT_IP and used to fill the IP field of ArtPollReply.
 * 0.0.0.0 until DHCP/link assigns one. */
static volatile uint32_t device_ip = 0;

/** Event handler for Ethernet link events (modern API, board-independent) */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
	switch (event_id)
	{
	case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "Ethernet link up");   break;
	case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Ethernet link down"); break;
	case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Ethernet started");   break;
	case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Ethernet stopped");   break;
	default: break;
	}
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
	ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
	const esp_netif_ip_info_t *ip_info = &event->ip_info;

	/* Stash the assigned address (network byte order) for ArtPollReply. */
	device_ip = ip_info->ip.addr;

	ESP_LOGI(TAG, "Ethernet Got IP Address");
	ESP_LOGI(TAG, "~~~~~~~~~~~");
	ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
	ESP_LOGI(TAG, "~~~~~~~~~~~");
}

void dmx_task()
{

	portMUX_TYPE myMutex = portMUX_INITIALIZER_UNLOCKED;

	uint32_t Cold = 0;
	uint32_t new_frame;
	uint32_t last_frame;

	uint32_t *GPIO_w1ts = (uint32_t *)0x3ff44008;
	uint32_t *GPIO_w1tc = (uint32_t *)0x3ff4400c;
	uint32_t outputH = 0;
	uint32_t outputL = 0;

	uint_fast16_t curbit = 0;
	uint_fast16_t curchan = 0;
	uint_fast8_t chanbit = 0;

	uint_fast8_t i = 0;

	uint_fast8_t chanval = 0;
	uint_fast8_t outbuf = 0;

	uint32_t bitmask = 0;
	for (uint_fast8_t j = 0; j < NUM_OUT; j++)
		bitmask |= 1u << GPIO_patch[j];

	gpio_config_t io_conf;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pin_bit_mask = bitmask;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	gpio_config(&io_conf);

	vPortEnterCritical(&myMutex);
	Cold = xthal_get_ccount();
	last_frame = Cold;

	while (1)
	{
		if (stopFlag)
		{
			vPortExitCritical(&myMutex);
			dmxStopped = 1;       /* ack: critical section released, safe to write flash */
			while (stopFlag)
				vTaskDelay(1);

			vPortEnterCritical(&myMutex);
			dmxStopped = 0;       /* back in the critical section */
			curbit = 0;
			Cold = xthal_get_ccount();
			last_frame = Cold;
		}

		outbuf = 0b00000000;
		if (curbit < BREAK)
		{
		}

		else if (curbit < BREAK + MAB)
		{
			outbuf = 0b11111111;
		}

		else if (curbit >= BREAK + MAB)
		{
			curchan = (curbit - BREAK - MAB) / 11;
			chanbit = (curbit - BREAK - MAB) % 11;

			if (chanbit == 0)
				outbuf = 0b00000000;

			else if ((chanbit == 9) || (chanbit == 10))
				outbuf = 0b11111111;

			else
			{
				for (i = 0; i < NUM_OUT; i++)
				{
					if (curchan == 0)
						chanval = 0;
					else
						chanval = DMXbuf[512 * DMX_repatch[i] + curchan - 1];
					if (!((chanval & (1 << (chanbit - 1))) == 0))
						outbuf |= (0b00000001 << i);
				}
			}
		}

		outputH = 0;
		outputL = 0;
		for (i = 0; i < NUM_OUT; i++)
		{
			outputH |= ((1 << i & outbuf) >> i) << GPIO_patch[i];
			outputL |= ((1 << i & ~outbuf) >> i) << GPIO_patch[i];
		}

		while ((xthal_get_ccount() - Cold) < 960)
		{
		};

		*GPIO_w1ts = outputH;
		*GPIO_w1tc = outputL;
		Cold += 960;

		curbit++;

		if (curbit >= (num_chan + 1) * 11 + BREAK + MAB)  /* 450 */
		{
			if ((synchronize == 1) && (trigger == 0) && (xthal_get_ccount() - last_frame < 50000000))
				curbit--;
			else
			{
				trigger = 0;
				curbit = 0;
				new_frame = xthal_get_ccount();
				refresh_rate = 1 / ((float)(new_frame - last_frame) * 0.000000004166666);
				last_frame = new_frame;
			}
		}
	}
}

void startDMX()
{
	stopFlag = 0;
}

void stopDMX()
{
	/* Clear the ack first so we wait for a *fresh* one raised after this
	 * stopFlag set — never a stale ack left over from the previous stop. */
	dmxStopped = 0;
	stopFlag = 1;
	/* Wait until dmx_task has actually left vPortExitCritical (it sets
	 * dmxStopped right after) before the caller writes flash. This is a real
	 * handshake, not a fixed-delay assumption: dmx_task checks stopFlag every
	 * bit-time (~4 us) so this normally returns within a tick. The timeout is a
	 * failsafe so a wedged dmx_task can never hang the config interface. */
	for (uint_fast8_t i = 0; !dmxStopped && i < 50; i++)
		vTaskDelay(1);
}

void save_dmx_patch()
{
	stopDMX();

	nvs_handle my_handle;
	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "save_dmx_patch: nvs_open failed");
		startDMX();   /* never leave DMX parked on a failed save */
		return;
	}
	nvs_set_blob(my_handle, "DMXPATCH", DMX_patch, NUM_OUT * sizeof(uint_fast16_t));
	nvs_commit(my_handle);
	nvs_close(my_handle);

	startDMX();
}

void save_sync_state()
{
	stopDMX();

	nvs_handle my_handle;
	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "save_sync_state: nvs_open failed");
		startDMX();   /* never leave DMX parked on a failed save */
		return;
	}
	nvs_set_u8(my_handle, "SYNC_STATE", (uint8_t)synchronize);
	nvs_set_u16(my_handle, "SYNC_ADDR", (uint16_t)sync_addr);
	nvs_set_u16(my_handle, "NUM_CHAN", (uint16_t)num_chan);
	nvs_commit(my_handle);
	nvs_close(my_handle);

	startDMX();
}

void load_dmx_patch()
{
	nvs_handle my_handle;

	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "load_dmx_patch: nvs_open failed, keeping zero patch");
		return;   /* DMX_patch is a zero-initialized global */
	}
	size_t required_size = NUM_OUT * sizeof(uint_fast16_t);
	nvs_get_blob(my_handle, "DMXPATCH", DMX_patch, &required_size);
	nvs_close(my_handle);
}

void load_sync_state()
{
	nvs_handle my_handle;
	/* Initialize to safe defaults: nvs_get_* only writes the destination on
	 * success, so on a fresh device / after nvs_flash_erase() (keys absent)
	 * these defaults are kept instead of leaving the values uninitialized.
	 * Default num_chan to a full 512-channel universe (a valid frame). */
	uint8_t sync_load = 0;
	uint16_t sync_addr_load = 0;
	uint16_t num_chan_load = 512;

	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "load_sync_state: nvs_open failed, keeping defaults");
	}
	else
	{
		nvs_get_u8(my_handle, "SYNC_STATE", &sync_load);
		nvs_get_u16(my_handle, "SYNC_ADDR", &sync_addr_load);
		nvs_get_u16(my_handle, "NUM_CHAN", &num_chan_load);
		nvs_close(my_handle);
	}

	synchronize = (uint_fast8_t)sync_load;
	sync_addr = (uint_fast16_t)sync_addr_load;
	num_chan = (uint_fast16_t)num_chan_load;
}

void update_dmx_ptr()
{
	memset(DMXbuf, 0, NUM_OUT*512);
	for (uint_fast8_t i = 0; i < NUM_OUT; i++)
	{
		uint_fast16_t tmp = DMX_patch[i];
		for (uint_fast8_t j = 0; j <= i; j++)
		{
			if (DMX_patch[j] == tmp)
			{
				DMX_repatch[i] = j;
				break;
			}
		}
	}
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
	n = sprintf(replyBuf + n, "\r\n\tOUTPUT\tADDRESS\r\n");

	for (uint_fast8_t i = 0; i < NUM_OUT; i++)
		n = n + sprintf(replyBuf + n, "\t%u:\t%u\r\n", i+1, DMX_patch[i]);

	n = n + sprintf(replyBuf + n, "\r\n");
	n = n + sprintf(replyBuf + n, "\tNUM_CHAN: %u\r\n", num_chan);
	n = n + sprintf(replyBuf + n, "\r\n");
	n = n + sprintf(replyBuf + n, "\tSYNC: %s\r\n", (synchronize ? "1" : "0"));
	n = n + sprintf(replyBuf + n, "\tSYNC_ADDR: %u\r\n", sync_addr);
	n = n + sprintf(replyBuf + n, "\r\n");
	n = n + sprintf(replyBuf + n, "\trefresh rate: %f Hz\r\n", refresh_rate);
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

	inputBuf = calloc(BUFLEN, sizeof(char));
	replyBuf = calloc(BUFLEN, sizeof(char));
	if (inputBuf == NULL || replyBuf == NULL)
	{
		ESP_LOGE(TAG, "tcp_task: buffer alloc failed");
		esp_restart();
	}

	while (1)
	{
		printf("init socket\n");

		vTaskDelay(1000 / portTICK_PERIOD_MS);   /* remove? */

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

			printf("waiting fot connection\n");


			socklen_t client_len = sizeof(client);
			client_fd = accept(server_fd, (struct sockaddr *)&client, &client_len);

			if (client_fd < 0)
				break;

			vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
			printf("connection established\n");

			memset(replyBuf, 0, BUFLEN);
			n = sprintf(replyBuf, "LICHTFETISCH ArtNet Rack\r\n\r\n");
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

					if (token == NULL)
					{
						/* line was only separators (spaces/CRLF) — ignore */
					}

					else if (strcmp(token, "PATCH") == 0)
					{
						uint_fast8_t i = 0;
						token = strtok(NULL, " ");
						while (token != NULL && i < NUM_OUT)
						{
							DMX_patch[i] = atoi(token);
							token = strtok(NULL, " ");
							i++;
						}
						save_dmx_patch();
						update_dmx_ptr();
					}

					else if (strcmp(token, "SYNC") == 0)
					{
						token = strtok(NULL, " ");
						if (token != NULL)
						{
							if (strcmp(token, "1") == 0)
								synchronize = 1;
							else
								synchronize = 0;
							save_sync_state();

							printf("sync state saved\n");
						}
					}

					else if (strcmp(token, "SYNC_ADDR") == 0)
					{
						token = strtok(NULL, " ");
						if (token != NULL)
						{
							sync_addr = atoi(token);
							save_sync_state();

							printf("sync state saved\n");
						}
					}

					else if (strcmp(token, "NUM_CHAN") == 0)
					{
						int input;
						token = strtok(NULL, " ");
						if (token != NULL)
						{
							input = atoi(token);
							if (1>input) input = 1;
							if (512<input) input = 512;
							num_chan = input;
							save_sync_state();

							printf("sync state saved\n");
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

	char *ArtNetBuf;
	char *replyBuf;

	struct sockaddr_in si_me, si_other;
	uint32_t s, slen = sizeof(si_other), recv_len;

	ArtNetBuf = calloc(BUFLEN, sizeof(char));
	replyBuf = calloc(BUFLEN, sizeof(char));
	if (ArtNetBuf == NULL || replyBuf == NULL)
	{
		ESP_LOGE(TAG, "eth_task: buffer alloc failed");
		esp_restart();
	}

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	memset((char *)&si_me, 0, sizeof(si_me));
	si_me.sin_family = AF_INET;
	si_me.sin_port = htons(PORT);
	si_me.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(s, (struct sockaddr *)&si_me, sizeof(si_me));

	while (1)
	{
		while (stopFlag)
			vTaskDelay(1);

		recv_len = recvfrom(s, ArtNetBuf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen);
		if (recv_len >= 10)
		{
			if (strcmp(ArtNetBuf, artnet_header) == 0)
			{
				opcode = ((uint8_t)ArtNetBuf[9] << 8) | (uint8_t)ArtNetBuf[8];
				if (opcode == 0x5000 && recv_len >= 18)
				{
					portaddress = ((uint8_t)ArtNetBuf[15] << 8) | (uint8_t)ArtNetBuf[14];
					bufaddress = -1;
					for (uint_fast8_t i = 0; i < NUM_OUT; i++)
					{
						if (DMX_patch[i] == portaddress)
						{
							bufaddress = i;
							break;
						}
					}
					if (bufaddress != -1)
					{
						DMXlength = ((uint8_t)ArtNetBuf[16] << 8) | (uint8_t)ArtNetBuf[17];
						/* Clamp before the copy: never exceed the 512-byte
						 * output slot (DMXbuf is NUM_OUT*512), and never read
						 * past what was actually received (data starts at
						 * offset 18). recv_len >= 18 is guaranteed above. */
						if (DMXlength > 512)
							DMXlength = 512;
						if (DMXlength > recv_len - 18)
							DMXlength = recv_len - 18;
						memcpy(DMXbuf + bufaddress * 512, ArtNetBuf + 18, DMXlength);
						if (synchronize == 1 && portaddress == sync_addr)
							trigger = 1;
					}
				}
				else if (opcode == 0x2000 || opcode == 0x6000 || opcode == 0x7000)
				{
					memset(replyBuf, 0, BUFLEN);
					memcpy(replyBuf, artnet_header, 8);
					replyBuf[8] = 0x00;
					replyBuf[9] = 0x21;

					uint32_t reply_ip = device_ip;   /* real device IP (NBO), 0.0.0.0 until link up */
					memcpy(replyBuf + 10, &reply_ip, 4);

					replyBuf[14] = 0x36;
					replyBuf[15] = 0x19;

					replyBuf[23] = 1 << 7 | 1 << 6 | 1 << 5;
					strcpy(replyBuf + 26, "LF Rack");
					strcpy(replyBuf + 44, "LICHTFETISCH ArtNet Rack");

					sendto(s, replyBuf, 239, 0, (struct sockaddr *)&si_other, slen);
				}
			}
		}
	}
}

void app_main(void)
{
	ESP_LOGI(TAG, "ArtNetNode FW %s | board=%s variant=%s", FIRMWARE_VERSION, BOARD_NAME, VARIANT_NAME);

	/* --- TCP/IP stack + default event loop --- */
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	/* --- Ethernet netif --- */
	esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
	esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
	if (eth_netif == NULL)
	{
		ESP_LOGE(TAG, "esp_netif_new failed");
		esp_restart();
	}

	/* --- Power the PHY (board-specific enable GPIO) --- */
	gpio_config_t phy_pwr = {
		.pin_bit_mask = 1ULL << PIN_PHY_POWER,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = 0,
		.pull_down_en = 0,
		.intr_type = GPIO_INTR_DISABLE
	};
	ESP_ERROR_CHECK(gpio_config(&phy_pwr));
	gpio_set_level((gpio_num_t)PIN_PHY_POWER, 1);
	vTaskDelay(pdMS_TO_TICKS(10));

	/* --- MAC + PHY (ESP32 EMAC + LAN87xx over RMII) --- */
	eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
	eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
	phy_cfg.phy_addr = ETH_PHY_ADDR;
	phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;

	eth_esp32_emac_config_t esp32_emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
	esp32_emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
	esp32_emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
	esp32_emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
	esp32_emac_cfg.clock_config.rmii.clock_mode = ETH_RMII_CLK_MODE;
	esp32_emac_cfg.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;

	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_cfg, &mac_cfg);
	esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

	esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
	esp_eth_handle_t eth_handle = NULL;
	ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));
	ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

	/* Register app event handlers (system handlers are auto-registered in v5.x) */
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
	                                           &eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
	                                           &got_ip_event_handler, NULL));

	/* Start Ethernet state machine */
	ESP_ERROR_CHECK(esp_eth_start(eth_handle));

	/* --- NVS --- */
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	/* --- DMX buffer + persisted settings --- */
	DMXbuf = calloc(NUM_OUT * 512, sizeof(uint8_t));
	if (DMXbuf == NULL)
	{
		ESP_LOGE(TAG, "DMXbuf alloc failed");
		esp_restart();
	}

	load_dmx_patch();
	update_dmx_ptr();
	load_sync_state();

	/* --- Tasks: network on core 0, DMX bit-bang alone on core 1 --- */
	if (xTaskCreatePinnedToCore(eth_task, "eth_task", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL, 0) != pdPASS ||
	    xTaskCreatePinnedToCore(tcp_task, "tcp_task", 2048, NULL, (tskIDLE_PRIORITY), NULL, 0) != pdPASS ||
	    xTaskCreatePinnedToCore(dmx_task, "dmx_task", 2018, NULL, (configMAX_PRIORITIES - 1), NULL, 1) != pdPASS)
	{
		ESP_LOGE(TAG, "task create failed");
		esp_restart();
	}
}
