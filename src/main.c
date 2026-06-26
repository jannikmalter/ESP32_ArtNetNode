/*
 * ESP32 ArtNetNode  —  Art-Net -> DMX512 (7 outputs, software bit-bang)
 *
 * The DMX engine, Art-Net/UDP handling and NVS logic are ported VERBATIM from the
 * original working OLIMEX ESP32-POE firmware (ESP-IDF ~v4.0). Only the Ethernet
 * bring-up has been modernized to the ESP-IDF v5 API and parameterized to build
 * for two boards, and one v4->v5 GPIO enum name was fixed (GPIO_PIN_INTR_DISABLE
 * -> GPIO_INTR_DISABLE in dmx_task). The original raw TCP config CLI has since
 * been removed in favour of the web UI (R23).
 *
 * Build configuration is selected at compile time via -D flags in
 * platformio.ini — see the BUILD CONFIGURATION block below.
 */

#define FIRMWARE_VERSION "1.9"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>            /* isxdigit() for the query-string URL decoder */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"        /* esp_timer_get_time() for the Art-Net pps window */
#include "esp_mac.h"          /* esp_read_mac() for the ArtPollReply MAC field (R22) */
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_ota_ops.h"      /* OTA foundation (R25/R26): dual-slot update + rollback */
#include "esp_partition.h"
#include "esp_http_server.h"  /* Web config UI (R23) — replaced the raw TCP CLI */
#include "web_assets.h"        /* index_html[] — generated from src/index.html */

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

#define BREAK 30
#define MAB 10

/* Art-Net opcodes (little-endian on the wire; see info.md constants section). */
#define OP_POLL      0x2000   /* ArtPoll      -> we reply with ArtPollReply       */
#define OP_POLLREPLY 0x2100   /* ArtPollReply -> our reply opcode                 */
#define OP_DMX       0x5000   /* ArtDmx       -> DMX data for one universe        */
#define OP_ADDRESS   0x6000   /* ArtAddress   -> remote config (patch/name) (R22) */
#define OP_INPUT     0x7000   /* ArtInput     -> input enable/disable (unhandled) */

static const char *TAG = "ArtNetNode";

uint8_t *DMXbuf;

uint_fast16_t DMX_patch[NUM_OUT] = {0, 0, 0, 0, 0, 0, 0};
uint_fast8_t DMX_repatch[NUM_OUT] = {0, 0, 0, 0, 0, 0, 0};

/* Cross-task reconfiguration handshake (no locks):
 *   stopFlag   - set via stopDMX() by the config/OTA paths, polled by dmx_task & eth_task.
 *   dmxStopped - set by dmx_task once it has left vPortExitCritical and is
 *                idling; lets stopDMX *observe* the critical-section exit
 *                instead of assuming a fixed delay was long enough.
 * Both are written by one task and read by another, so they must be volatile
 * or the compiler may cache them in a register and never see the change.
 *
 * stopFlag starts SET: DMX (and Art-Net input) are parked through the whole of
 * app_main and only go live once boot has fully succeeded (app_main calls
 * startDMX() last). This keeps every boot-time flash op safe — the bit-bang
 * generator never holds core 1 in its interrupt-disabled critical section while
 * the cache is being stopped — and means a boot that faults out never drives the
 * DMX lines with half-initialised state. */
volatile uint_fast8_t stopFlag = 1;
volatile uint_fast8_t dmxStopped = 0;

uint_fast8_t synchronize = 0;
uint_fast16_t sync_addr = 0;
uint_fast16_t num_chan = 0;
volatile uint_fast8_t trigger = 0;

/* Node identity reported in ArtPollReply and settable over Art-Net via ArtAddress
 * (R22). Seeded from the build VARIANT_NAME so a "mini" build no longer announces
 * "Rack" (B12); overridden by NVS (keys SHORTNAME/LONGNAME) and by ArtAddress.
 * ShortName max 17 chars + null, LongName max 63 + null (Art-Net field sizes). */
char node_short_name[18] = "LF " VARIANT_NAME;
char node_long_name[64]  = "LICHTFETISCH ArtNet " VARIANT_NAME;

float refresh_rate;

/* Art-Net packets/sec, recomputed once a second in eth_task (R28). Read by the
 * web UI via /api/state to drive the live traffic graph. Decays to 0 when Art-Net
 * stops (the socket has a recv timeout so the window still fires). */
volatile uint_fast16_t artnet_pps = 0;

/* Core-0 CPU utilization 0..100 %, recomputed once a second by stats_task from
 * the core-0 idle task's run-time (R29). Read by the web UI via /api/state. Core 1
 * is intentionally not measured (dmx_task owns it via a forever critical section,
 * so its idle task never runs). */
volatile uint_fast8_t cpu_load0 = 0;

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

/* Node name (R22). Like load_sync_state, nvs_get_str only writes on success, so a
 * fresh device keeps the VARIANT_NAME-seeded defaults. The size args are in/out;
 * pass the full buffer sizes. */
void load_node_name()
{
	nvs_handle my_handle;
	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "load_node_name: nvs_open failed, keeping defaults");
		return;
	}
	size_t short_size = sizeof(node_short_name);
	size_t long_size  = sizeof(node_long_name);
	nvs_get_str(my_handle, "SHORTNAME", node_short_name, &short_size);
	nvs_get_str(my_handle, "LONGNAME", node_long_name, &long_size);
	nvs_close(my_handle);
}

/* Persist the node name. Reuses the stopDMX()->NVS->startDMX() handshake so the
 * flash write never races the core-1 critical section (same contract as the patch
 * and sync save paths). */
void save_node_name()
{
	stopDMX();

	nvs_handle my_handle;
	if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle) != ESP_OK)
	{
		ESP_LOGE(TAG, "save_node_name: nvs_open failed");
		startDMX();   /* never leave DMX parked on a failed save */
		return;
	}
	nvs_set_str(my_handle, "SHORTNAME", node_short_name);
	nvs_set_str(my_handle, "LONGNAME", node_long_name);
	nvs_commit(my_handle);
	nvs_close(my_handle);

	startDMX();
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

/* ===========================================================================
 *  NATIVE ART-NET CONFIG & QUERY  (R22)
 *
 *  ArtPollReply reports each output as its own bound port (Art-Net 4 per-port
 *  binding: one universe per reply, NumPortsLo=1, BindIndex 1..NUM_OUT) so every
 *  output advertises a fully independent 15-bit Port-Address. ArtAddress lets a
 *  controller set an output's universe and the node name remotely, persisted via
 *  the same stopDMX()->NVS->startDMX() handshake the web UI uses.
 *
 *  15-bit Port-Address = NetSwitch(7) : SubSwitch(4) : Sw(4). DMX_patch[i] holds
 *  the full value; the three nibble groups map onto the ArtPollReply/ArtAddress
 *  NetSwitch / SubSwitch / SwOut fields.
 * =========================================================================== */

#define ARTNET_REPLY_LEN 239   /* >= 207 minimum; covers through GoodOutputB */

/* Send one unicast ArtPollReply per output (BindIndex 1..NUM_OUT). dst is the
 * poller; replyBuf is the caller's BUFLEN scratch buffer. */
static void send_artpollreply(int s, char *replyBuf, struct sockaddr_in *dst, uint32_t slen)
{
	static uint8_t mac[6];
	static uint_fast8_t mac_read = 0;
	if (!mac_read)
	{
		if (esp_read_mac(mac, ESP_MAC_ETH) != ESP_OK)
			memset(mac, 0, sizeof(mac));   /* zero is legal per spec */
		mac_read = 1;
	}

	const char *dot = strchr(FIRMWARE_VERSION, '.');

	for (uint_fast8_t k = 0; k < NUM_OUT; k++)
	{
		uint_fast16_t addr = DMX_patch[k];

		memset(replyBuf, 0, ARTNET_REPLY_LEN);
		memcpy(replyBuf, "Art-Net", 8);
		replyBuf[8] = (uint8_t)(OP_POLLREPLY & 0xFF);   /* low byte first */
		replyBuf[9] = (uint8_t)(OP_POLLREPLY >> 8);

		memcpy(replyBuf + 10, (const void *)&device_ip, 4);   /* IP (NBO) */
		replyBuf[14] = 0x36;   /* Port 0x1936, low byte first */
		replyBuf[15] = 0x19;

		replyBuf[16] = (uint8_t)atoi(FIRMWARE_VERSION);       /* VersInfoH */
		replyBuf[17] = (uint8_t)(dot ? atoi(dot + 1) : 0);    /* VersInfoL */

		replyBuf[18] = (uint8_t)((addr >> 8) & 0x7F);   /* NetSwitch  (bits 14-8) */
		replyBuf[19] = (uint8_t)((addr >> 4) & 0x0F);   /* SubSwitch  (bits 7-4)  */

		replyBuf[23] = 0xE0;   /* Status1: indicators normal + network prog authority */

		strncpy(replyBuf + 26, node_short_name, 17);    /* ShortName[18] */
		strncpy(replyBuf + 44, node_long_name, 63);     /* LongName[64]  */

		replyBuf[173] = 1;      /* NumPortsLo (NumPortsHi=0): one port per reply */
		replyBuf[174] = 0x80;   /* PortTypes[0]: output, DMX512 */
		replyBuf[182] = 0x80;   /* GoodOutputA[0]: data being output */
		replyBuf[190] = (uint8_t)(addr & 0x0F);   /* SwOut[0] (bits 3-0) */

		replyBuf[200] = 0x00;   /* Style: StNode */
		memcpy(replyBuf + 201, mac, 6);                 /* MAC[6] */
		memcpy(replyBuf + 207, (const void *)&device_ip, 4);   /* BindIp */
		replyBuf[211] = (uint8_t)(k + 1);   /* BindIndex 1..NUM_OUT */
		replyBuf[212] = 0x0D;   /* Status2: web-config + 15-bit Port-Address + DHCP-capable */

		sendto(s, replyBuf, ARTNET_REPLY_LEN, 0, (struct sockaddr *)dst, slen);
	}
}

/* Apply an ArtAddress packet (OpCode 0x6000): set one output's universe and/or the
 * node name, persist, then reply with ArtPollReply. buf/recv_len are the received
 * datagram; the reply uses the caller's replyBuf and dst. */
static void handle_artaddress(const char *buf, int recv_len, int s, char *replyBuf,
                              struct sockaddr_in *dst, uint32_t slen)
{
	if (recv_len < 107)   /* full ArtAddress is 107 bytes (Command at offset 106) */
		return;

	/* BindIndex (offset 13) selects the output; 0 or 1 == root == output 0. */
	uint_fast8_t bind = (uint8_t)buf[13];
	uint_fast8_t idx = (bind <= 1) ? 0 : (uint_fast8_t)(bind - 1);
	if (idx >= NUM_OUT)
		return;

	/* Rebuild the targeted output's Port-Address from the current value, replacing
	 * only the sub-fields whose source byte has bit7 set (spec: "ignored unless bit
	 * 7 is high"). NetSwitch=offset 12, SubSwitch=offset 104, SwOut[0]=offset 100. */
	uint_fast8_t netsw = (uint8_t)buf[12];
	uint_fast8_t subsw = (uint8_t)buf[104];
	uint_fast8_t swout = (uint8_t)buf[100];
	uint_fast16_t addr = DMX_patch[idx];
	if (netsw & 0x80) addr = (uint_fast16_t)((addr & 0x00FF) | ((netsw & 0x7F) << 8));
	if (subsw & 0x80) addr = (uint_fast16_t)((addr & 0xFF0F) | ((subsw & 0x0F) << 4));
	if (swout & 0x80) addr = (uint_fast16_t)((addr & 0xFFF0) | (swout & 0x0F));

	uint_fast8_t patch_changed = (addr != DMX_patch[idx]);
	if (patch_changed)
		DMX_patch[idx] = addr;

	/* PortName (offset 14, 18 bytes) and LongName (offset 32, 64 bytes): the node
	 * ignores a field whose first byte is null. */
	uint_fast8_t name_changed = 0;
	if (buf[14] != '\0')
	{
		strncpy(node_short_name, buf + 14, 17);
		node_short_name[17] = '\0';
		name_changed = 1;
	}
	if (buf[32] != '\0')
	{
		strncpy(node_long_name, buf + 32, 63);
		node_long_name[63] = '\0';
		name_changed = 1;
	}

	/* Persist through the proven handshake. PATCH order: save (stops/starts DMX),
	 * then rebuild the dedup repatch table (matches /api/config). */
	if (patch_changed)
	{
		/* Sync mode and channel count cannot be set over Art-Net. Reset them to
		 * defaults (free-run, full 512-channel frame) on any Art-Net patch change,
		 * so a node configured solely via Art-Net behaves predictably instead of
		 * silently keeping stale web-UI values the user can't see here. */
		synchronize = 0;
		sync_addr = 0;
		num_chan = 512;
		save_sync_state();

		save_dmx_patch();
		update_dmx_ptr();
	}
	if (name_changed)
		save_node_name();

	/* Command byte (offset 106): AcNone / AcLed* / AcCancelMerge etc. are not
	 * implemented on this output-only node; ignore safely (no error). */

	send_artpollreply(s, replyBuf, dst, slen);   /* spec: node replies with ArtPollReply */
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
	int s, recv_len;                 /* signed: recvfrom returns -1 on timeout/error (B7) */
	uint32_t slen = sizeof(si_other);

	/* Art-Net packets/sec window (R28): count packets and, once >1 s has elapsed,
	 * publish the count and restart the window. */
	uint_fast16_t pps_count = 0;
	int64_t pps_t0 = esp_timer_get_time();

	ArtNetBuf = calloc(BUFLEN, sizeof(char));
	replyBuf = calloc(BUFLEN, sizeof(char));
	if (ArtNetBuf == NULL || replyBuf == NULL)
	{
		ESP_LOGE(TAG, "eth_task: buffer alloc failed");
		esp_restart();
	}

	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0)
	{
		ESP_LOGE(TAG, "eth_task: socket() failed");
		esp_restart();
	}

	/* Wake recvfrom every 250 ms even with no traffic, so the packets/sec window
	 * below still runs during silence and the rate decays to 0 (R28) instead of
	 * holding the last value — lets the web UI show "no packets reaching the node".
	 * recvfrom then returns -1 (EAGAIN) on each idle tick, hence the signed
	 * recv_len + the >= 10 guard (B7). */
	struct timeval rcvto = { .tv_sec = 0, .tv_usec = 250000 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

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

		/* Publish packets/sec once per second, whether or not a packet arrived, so
		 * the rate falls to 0 when Art-Net stops (R28). */
		int64_t pps_now = esp_timer_get_time();
		if (pps_now - pps_t0 >= 1000000)
		{
			artnet_pps = pps_count;
			pps_count = 0;
			pps_t0 = pps_now;
		}

		if (recv_len >= 10)
		{
			if (strcmp(ArtNetBuf, artnet_header) == 0)
			{
				pps_count++;   /* count every Art-Net packet (R28) */

				opcode = ((uint8_t)ArtNetBuf[9] << 8) | (uint8_t)ArtNetBuf[8];
				if (opcode == OP_DMX && recv_len >= 18)
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
				else if (opcode == OP_POLL)
				{
					/* Discovery: report every output as its own bound port (R22). */
					send_artpollreply(s, replyBuf, &si_other, slen);
				}
				else if (opcode == OP_ADDRESS)
				{
					/* Remote config: set output universe / node name (R22). */
					handle_artaddress(ArtNetBuf, recv_len, s, replyBuf, &si_other, slen);
				}
				/* OP_INPUT (0x7000) intentionally unhandled: output-only node.
				 * The old firmware mis-used 0x6000/0x7000 as poll triggers; that
				 * quirk is gone now that they have their correct meaning. */
			}
		}
	}
}

/* Core-0 CPU load monitor (R29). FreeRTOS run-time stats accumulate per-task busy
 * time in esp_timer microseconds; core-0's idle task therefore accrues run-time
 * only while core 0 is idle. Sampling it over a 1 s wall window gives
 *   load0 = 100 * (1 - idle_busy / wall).
 * No calibration needed (same time base), and the unsigned counter delta is taken
 * modulo its width so a 32-bit wrap is harmless. Core 1 is deliberately not
 * measured: dmx_task owns it via a forever critical section, so its idle task
 * never runs and any scheduler-based figure is meaningless.
 *
 * This (non-SMP) IDF kernel has no ulTaskGetRunTimeCounter(), so the per-task
 * run-time is read from a uxTaskGetSystemState() snapshot, matched by handle. */
#define STATS_MAX_TASKS 32
static configRUN_TIME_COUNTER_TYPE idle_runtime(TaskHandle_t idle, TaskStatus_t *st, UBaseType_t n)
{
	for (UBaseType_t i = 0; i < n; i++)
		if (st[i].xHandle == idle)
			return st[i].ulRunTimeCounter;
	return 0;
}

void stats_task(void *arg)
{
	static TaskStatus_t st[STATS_MAX_TASKS];   /* ~1.4 KB, kept off the task stack */
	TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
	configRUN_TIME_COUNTER_TYPE total;

	UBaseType_t n = uxTaskGetSystemState(st, STATS_MAX_TASKS, &total);
	configRUN_TIME_COUNTER_TYPE prev_idle = idle_runtime(idle0, st, n);
	int64_t prev_t = esp_timer_get_time();

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));

		n = uxTaskGetSystemState(st, STATS_MAX_TASKS, &total);
		if (n == 0 || idle0 == NULL)   /* array too small / no idle handle */
			continue;

		configRUN_TIME_COUNTER_TYPE now_idle = idle_runtime(idle0, st, n);
		int64_t now_t = esp_timer_get_time();
		uint64_t idle_dt = (uint64_t)(now_idle - prev_idle);   /* busy us, modular */
		int64_t  wall_dt = now_t - prev_t;                     /* elapsed us */

		int load = 0;
		if (wall_dt > 0)
		{
			load = 100 - (int)((idle_dt * 100) / (uint64_t)wall_dt);
			if (load < 0)   load = 0;
			if (load > 100) load = 100;
		}
		cpu_load0 = (uint_fast8_t)load;

		prev_idle = now_idle;
		prev_t = now_t;
	}
}

/* ===========================================================================
 *  WEB CONFIG UI  (R23 / T8)
 *
 *  A minimal esp_http_server, pinned to core 0 so it never touches the DMX core.
 *  It is the sole config interface (the raw TCP CLI it replaced has been removed)
 *  and reuses the exact same stopDMX() -> write NVS -> startDMX() save handshake
 *  (save_dmx_patch / save_sync_state), so it never mutates live DMX state directly.
 *
 *  index.html is embedded into flash via EMBED_TXTFILES (see src/CMakeLists.txt).
 *  This server also hosts the OTA upload endpoint (R26): the firmware-upload
 *  handler registers on this same server instance.
 * =========================================================================== */

/* Escape a string into dst as a JSON string body (no surrounding quotes): the
 * node names can be set to arbitrary bytes over Art-Net, so a stray " \ or
 * control char must never break the /api/state JSON. Always NUL-terminates and
 * never writes past dstlen (a name too long to fit is truncated, not overrun). */
static void json_escape(const char *src, char *dst, int dstlen)
{
	int j = 0;
	for (; *src && j < dstlen - 7; src++)   /* worst case adds \uXXXX (6) + NUL */
	{
		unsigned char c = (unsigned char)*src;
		switch (c)
		{
		case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
		case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
		case '\b': dst[j++] = '\\'; dst[j++] = 'b';  break;
		case '\f': dst[j++] = '\\'; dst[j++] = 'f';  break;
		case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
		case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
		case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
		default:
			if (c < 0x20)
				j += sprintf(dst + j, "\\u%04x", c);
			else
				dst[j++] = (char)c;
		}
	}
	dst[j] = '\0';
}

/* Flat JSON snapshot of all settings + live refresh rate (read by index.html). */
static int build_state_json(char *buf, int len)
{
	char sname[128], lname[400];   /* escaped: up to 6x the 17/63-byte names */
	json_escape(node_short_name, sname, sizeof(sname));
	json_escape(node_long_name, lname, sizeof(lname));
	int n = snprintf(buf, len, "{\"fw\":\"%s\",\"board\":\"%s\",\"variant\":\"%s\",\"name\":\"%s\",\"lname\":\"%s\",\"patch\":[",
	                 FIRMWARE_VERSION, BOARD_NAME, VARIANT_NAME, sname, lname);
	for (uint_fast8_t i = 0; i < NUM_OUT; i++)
		n += snprintf(buf + n, len - n, "%s%u", i ? "," : "", (unsigned)DMX_patch[i]);
	n += snprintf(buf + n, len - n,
	              "],\"sync\":%u,\"sync_addr\":%u,\"num_chan\":%u,\"refresh\":%.1f,\"pps\":%u,\"load0\":%u}",
	              (unsigned)synchronize, (unsigned)sync_addr, (unsigned)num_chan,
	              refresh_rate, (unsigned)artnet_pps, (unsigned)cpu_load0);
	return n;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
	char buf[640];
	int n = build_state_json(buf, sizeof(buf));
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, buf, n);
}

/* In-place percent-decode of a query-string value ('+' -> space, %XX -> byte).
 * httpd_query_key_value does not decode, so names with spaces/punctuation arrive
 * encoded; the web UI encodeURIComponent()s them and we undo it here. */
static void url_decode(char *s)
{
	char *d = s;
	for (; *s; s++)
	{
		if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2]))
		{
			char h[3] = { s[1], s[2], '\0' };
			*d++ = (char)strtol(h, NULL, 16);
			s += 2;
		}
		else
			*d++ = (*s == '+') ? ' ' : *s;
	}
	*d = '\0';
}

/* POST /api/config?patch=a,b,c,d,e,f,g&sync=0|1&sync_addr=n&num_chan=n&sname=..&lname=..
 * Any subset of keys may be present; applies each via the shared save path, then
 * returns the fresh state JSON. */
static esp_err_t config_post_handler(httpd_req_t *req)
{
	char query[512];
	char val[200];
	uint_fast8_t save_patch = 0, save_sync = 0, save_name = 0;

	if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
	{
		if (httpd_query_key_value(query, "patch", val, sizeof(val)) == ESP_OK)
		{
			char *save;
			uint_fast8_t i = 0;
			for (char *tok = strtok_r(val, ",", &save); tok != NULL && i < NUM_OUT;
			     tok = strtok_r(NULL, ",", &save))
				DMX_patch[i++] = atoi(tok);
			save_patch = 1;
		}
		if (httpd_query_key_value(query, "sync", val, sizeof(val)) == ESP_OK)
		{
			synchronize = (atoi(val) != 0);
			save_sync = 1;
		}
		if (httpd_query_key_value(query, "sync_addr", val, sizeof(val)) == ESP_OK)
		{
			sync_addr = atoi(val);
			save_sync = 1;
		}
		if (httpd_query_key_value(query, "num_chan", val, sizeof(val)) == ESP_OK)
		{
			int n = atoi(val);
			if (n < 1) n = 1;
			if (n > 512) n = 512;
			num_chan = n;
			save_sync = 1;
		}
		/* Node identity (same length caps as the ArtAddress path: 17 / 63). */
		if (httpd_query_key_value(query, "sname", val, sizeof(val)) == ESP_OK)
		{
			url_decode(val);
			strncpy(node_short_name, val, 17);
			node_short_name[17] = '\0';
			save_name = 1;
		}
		if (httpd_query_key_value(query, "lname", val, sizeof(val)) == ESP_OK)
		{
			url_decode(val);
			strncpy(node_long_name, val, 63);
			node_long_name[63] = '\0';
			save_name = 1;
		}
	}

	/* Reuse the proven stop/start + NVS handshake. PATCH order matters:
	 * save (which stops/starts DMX), then rebuild the dedup repatch table. */
	if (save_patch)
	{
		save_dmx_patch();
		update_dmx_ptr();
	}
	if (save_sync)
		save_sync_state();
	if (save_name)
		save_node_name();

	char buf[640];
	int n = build_state_json(buf, sizeof(buf));
	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, buf, n);
}

/* POST /api/ota — stream a raw firmware image (octet-stream request body) into
 * the inactive OTA slot, then reboot into it. DMX is stopped for the *whole*
 * update (deliberately — never flash during a show): with core 1 parked outside
 * its critical section the flash writes can't race the cycle-counted bit-bang,
 * and there is no DMX output until the device comes back up. On any error we
 * abort the write and restart DMX; on success we reboot, handing off to the
 * rollback foundation in app_main (an image that fails to confirm reverts to the
 * previous slot, so a bad upload can't brick the node). ESP-IDF only (R7/R26). */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
	const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
	if (part == NULL)
	{
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "OTA: receiving %d bytes -> %s", req->content_len, part->label);
	stopDMX();   /* park core 1 for the whole update (flash cache stops on both cores) */

	esp_ota_handle_t ota = 0;
	if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota) != ESP_OK)
	{
		startDMX();
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
		return ESP_FAIL;
	}

	char buf[1024];
	int recv, total = 0;
	while ((recv = httpd_req_recv(req, buf, sizeof(buf))) > 0)
	{
		if (esp_ota_write(ota, buf, recv) != ESP_OK)
		{
			esp_ota_abort(ota);
			startDMX();
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
			return ESP_FAIL;
		}
		total += recv;
	}
	if (recv < 0)   /* socket error / timeout mid-stream */
	{
		esp_ota_abort(ota);
		startDMX();
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
		return ESP_FAIL;
	}

	if (esp_ota_end(ota) != ESP_OK)   /* validates image magic / size / hash */
	{
		startDMX();
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
		return ESP_FAIL;
	}
	if (esp_ota_set_boot_partition(part) != ESP_OK)
	{
		startDMX();
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "OTA: %d bytes written, rebooting into %s", total, part->label);
	httpd_resp_sendstr(req, "OK");
	vTaskDelay(pdMS_TO_TICKS(500));   /* let the response flush before we reset */
	esp_restart();
	return ESP_OK;   /* not reached */
}

static void start_webserver(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.core_id = 0;            /* keep the HTTP task off core 1 (DMX) */
	config.stack_size = 8192;
	config.lru_purge_enable = true;

	httpd_handle_t server = NULL;
	if (httpd_start(&server, &config) != ESP_OK)
	{
		ESP_LOGE(TAG, "httpd_start failed");
		return;
	}

	static const httpd_uri_t root  = { .uri = "/",           .method = HTTP_GET,  .handler = root_get_handler };
	static const httpd_uri_t state = { .uri = "/api/state",  .method = HTTP_GET,  .handler = state_get_handler };
	static const httpd_uri_t cfg   = { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler };
	static const httpd_uri_t ota   = { .uri = "/api/ota",    .method = HTTP_POST, .handler = ota_post_handler };
	httpd_register_uri_handler(server, &root);
	httpd_register_uri_handler(server, &state);
	httpd_register_uri_handler(server, &cfg);
	httpd_register_uri_handler(server, &ota);

	ESP_LOGI(TAG, "Web UI started on http://<device-ip>/");
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
	load_node_name();

	/* --- Tasks: network on core 0, DMX bit-bang alone on core 1 --- */
	if (xTaskCreatePinnedToCore(eth_task, "eth_task", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL, 0) != pdPASS ||
	    xTaskCreatePinnedToCore(dmx_task, "dmx_task", 2018, NULL, (configMAX_PRIORITIES - 1), NULL, 1) != pdPASS)
	{
		ESP_LOGE(TAG, "task create failed");
		esp_restart();
	}

	/* --- Web config UI (core 0); also the host for the future OTA endpoint --- */
	start_webserver();

	/* --- Core-0 CPU load monitor (R29), core 0, just above idle. Diagnostic, so a
	 * failure here is logged but not fatal. --- */
	if (xTaskCreatePinnedToCore(stats_task, "stats_task", 2048, NULL, (tskIDLE_PRIORITY + 1), NULL, 0) != pdPASS)
		ESP_LOGW(TAG, "stats_task create failed; core-0 load unavailable");

	/* --- OTA rollback confirm (R25/R26 foundation) ---
	 * A freshly OTA-written image boots in the PENDING_VERIFY state; the
	 * bootloader rolls it back to the previous slot on the next reset unless we
	 * mark it valid. Reaching this point means boot, Ethernet bring-up and the
	 * DMX/network tasks all started, so the image is good — confirm it.
	 * USB-flashed images are in the UNDEFINED state and are left untouched.
	 * (The OTA write path that produces such images lands with T10/T11.)
	 *
	 * These calls touch flash, which disables the flash cache on *both* cores.
	 * DMX has been parked since init (stopFlag), so dmx_task is idling outside its
	 * interrupt-disabled critical section and the cross-core cache stop completes
	 * normally. Wait for the parked ack first so the flash op can never race the
	 * one-time window where dmx_task is briefly inside the critical section at
	 * startup before it observes stopFlag. */
	for (uint_fast8_t i = 0; !dmxStopped && i < 50; i++)
		vTaskDelay(1);
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t ota_state;
	if (running != NULL &&
	    esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
	    ota_state == ESP_OTA_IMG_PENDING_VERIFY)
	{
		esp_ota_mark_app_valid_cancel_rollback();
		ESP_LOGI(TAG, "OTA image confirmed valid (rollback cancelled)");
	}

	/* Boot fully succeeded — go live: this is the only place DMX output and
	 * Art-Net input are enabled (everything above ran with them parked). */
	startDMX();
	ESP_LOGI(TAG, "boot complete, DMX output enabled");
}
