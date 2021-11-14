
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include <esp_log.h>
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include <sys/socket.h>
#include "freertos/event_groups.h"
#include "decode_image.h"
const static char *TAG = "LCD JPEG Effect Demo";
uint16_t **pixels = NULL;
#define LCD_HOST VSPI_HOST
#define DMA_CHAN 1

#define PIN_NUM_MISO 12
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 14
#define PIN_NUM_CS 15

#define PIN_NUM_DC 2
#define PIN_NUM_RST -1
#define PIN_NUM_BCKL -1

#define EXAMPLE_ESP_WIFI_SSID "carcar"
#define EXAMPLE_ESP_WIFI_PASS "22345678"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_MAX_STA_CONN 1
//为了加快SPI传输速度，每次SPI会通过DMA传输很多行一起显示，这里定义一次传输多少行，数字越大，占用内存越多，但传输越快，
//值要可以被240整除，16行，传15次一屏
#define PARALLEL_LINES 16

#define TCP_PORT 5555 // 监听客户端端口

EventGroupHandle_t tcp_event_group; // wifi建立成功信号量
// socket
static int server_socket = 0;					   // 服务器socket
static struct sockaddr_in server_addr;			   // server地址
static struct sockaddr_in client_addr;			   // client地址
static unsigned int socklen = sizeof(client_addr); // 地址长度
static int connect_socket = 0;					   // 连接socket
bool g_rxtx_need_restart = false;				   // 异常后，重新连接标记
// FreeRTOS event group to signal when we are connected to wifi
#define WIFI_CONNECTED_BIT BIT0

void close_socket();									   // 关闭socket
void recv_data(void *pvParameters);						   // 接收数据任务
int get_socket_error_code(int socket);					   // 获取socket错误代码
int show_socket_error_reason(const char *str, int socket); // 获取socket错误原因
int check_working_socket();								   // 检查socket
void wifi_init_softap();								   // WIFI作为AP的初始化
// 接收数据任务
unsigned char *Imgbuff;
static unsigned char tcpReceive[20000];
int currentIndex = 0;
unsigned char sendbuff[20];
spi_device_handle_t spi;
void po2(int size);

unsigned char crc8_compute(const unsigned char *pdata, unsigned data_size,
						   unsigned char crc_in)
{
	uint8_t cnt;
	uint8_t crc_poly = 0x07;
	uint8_t data_tmp = 0;

	while (data_size--)
	{
		data_tmp = *(pdata++);
		crc_in ^= (data_tmp << 0);

		for (cnt = 0; cnt < 8; cnt++)
		{
			if (crc_in & 0x80)
			{
				crc_in = (crc_in << 1) ^ crc_poly;
			}
			else
			{
				crc_in = crc_in << 1;
			}
		}
	}

	return crc_in;
}

void poces()
{
	int size = currentIndex + 1;
	int len;
	int con = 0;
	while (1)
	{
		if (currentIndex < 11)
		{
			return;
		}
		size = currentIndex + 1;
		con = 0;
		for (int i = 0; i < size - 10; i++)
		{
			if (tcpReceive[i] == 0xa5 &&
				tcpReceive[i + 1] == ((unsigned char)(~tcpReceive[i + 2])))
			{

				len = tcpReceive[i + 6] + 256 * tcpReceive[i + 7];

				ESP_LOGI(TAG, "getittjepg  %d", len);
				if (i + 11 + len <= size)
				{
					Imgbuff = tcpReceive + i;

					if (crc8_compute(Imgbuff, len + 10, 0) == Imgbuff[len + 10])
					{

						po2(i);
						currentIndex = 0;

						return;
					}
				}
			}
		}
		if (con == 0)
		{
			return;
		}
	}
}
void ReplyFileData(unsigned char *contents, int len, unsigned char *mother)
{
	mother[0] = (unsigned char)0xA5;
	mother[1] = (unsigned char)0x22;
	mother[2] = (unsigned char)~0x22;
	mother[3] = (unsigned char)0;
	mother[4] = 0;
	mother[5] = 0;

	mother[6] = len & 0xff;
	mother[7] = (len >> 8) & 0xff;
	mother[8] = (len >> 16) & 0xff;
	mother[9] = (len >> 24) & 0xff;
	for (int k = 0; k < len; k++)
	{
		mother[10 + k] = contents[k];
	}
	mother[10 + len] = crc8_compute(mother, 10 + len, 0);
}
void recv_data(void *pvParameters)
{
	int len = 0;
	int k;

	while (1)
	{
		len = recv(connect_socket, tcpReceive + currentIndex, 4000, 0); //读取接收数据
		g_rxtx_need_restart = false;
		if (len > 0)
		{

			currentIndex += len;
			if (currentIndex >= 18000)
			{
				currentIndex = 0;
			}
			poces();
			ESP_LOGI(TAG, "tcp seg  %d", len);
		}
		else
		{
			show_socket_error_reason("recv_data", connect_socket); //打印错误信息
			g_rxtx_need_restart = true;							   //服务器故障，标记重连
			vTaskDelete(NULL);
		}
	}
	close_socket();
	g_rxtx_need_restart = true; //标记重连
	vTaskDelete(NULL);
}
int noReceive = 0;
void send_data(void *pvParameters)
{
	vTaskDelay(5000 / portTICK_RATE_MS);
	while (1)
	{
		if (noReceive)
		{
			ReplyFileData(NULL, 0, sendbuff);
			send(connect_socket, sendbuff, 11, 0);
		}
		else
		{
			noReceive = 1;
		}

		vTaskDelay(5000 / portTICK_RATE_MS);
	}
}

// 建立tcp server
esp_err_t create_tcp_server(bool isCreatServer)
{
	//首次建立server
	if (isCreatServer)
	{
		ESP_LOGI(TAG, "server socket....,port=%d", TCP_PORT);
		server_socket = socket(AF_INET, SOCK_STREAM, 0); //新建socket
		if (server_socket < 0)
		{
			show_socket_error_reason("create_server", server_socket);
			close(server_socket); //新建失败后，关闭新建的socket，等待下次新建
			return ESP_FAIL;
		}
		//配置新建server socket参数
		server_addr.sin_family = AF_INET;
		server_addr.sin_port = htons(TCP_PORT);
		server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		// bind:地址的绑定
		if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
		{
			show_socket_error_reason("bind_server", server_socket);
			close(server_socket); // bind失败后，关闭新建的socket，等待下次新建
			return ESP_FAIL;
		}
	}
	// listen，下次时，直接监听
	if (listen(server_socket, 5) < 0)
	{
		show_socket_error_reason("listen_server", server_socket);
		close(server_socket); // listen失败后，关闭新建的socket，等待下次新建
		return ESP_FAIL;
	}
	// accept，搜寻全连接队列
	connect_socket = accept(server_socket, (struct sockaddr *)&client_addr, &socklen);
	if (connect_socket < 0)
	{
		show_socket_error_reason("accept_server", connect_socket);
		close(server_socket); // accept失败后，关闭新建的socket，等待下次新建
		return ESP_FAIL;
	}
	ESP_LOGI(TAG, "tcp connection established!");
	return ESP_OK;
}

// 获取socket错误代码
int get_socket_error_code(int socket)
{
	int result;
	u32_t optlen = sizeof(int);
	int err = getsockopt(socket, SOL_SOCKET, SO_ERROR, &result, &optlen);
	if (err == -1)
	{
		// WSAGetLastError();
		ESP_LOGE(TAG, "socket error code:%d", err);
		ESP_LOGE(TAG, "socket error code:%s", strerror(err));
		return -1;
	}
	return result;
}

// 获取socket错误原因
int show_socket_error_reason(const char *str, int socket)
{
	int err = get_socket_error_code(socket);
	if (err != 0)
	{
		ESP_LOGW(TAG, "%s socket error reason %d %s", str, err, strerror(err));
	}
	return err;
}
// 检查socket
int check_working_socket()
{
	int ret;
#if EXAMPLE_ESP_TCP_MODE_SERVER
	ESP_LOGD(TAG, "check server_socket");
	ret = get_socket_error_code(server_socket);
	if (ret != 0)
	{
		ESP_LOGW(TAG, "server socket error %d %s", ret, strerror(ret));
	}
	if (ret == ECONNRESET)
	{
		return ret;
	}
#endif
	ESP_LOGD(TAG, "check connect_socket");
	ret = get_socket_error_code(connect_socket);
	if (ret != 0)
	{
		ESP_LOGW(TAG, "connect socket error %d %s", ret, strerror(ret));
	}
	if (ret != 0)
	{
		return ret;
	}
	return 0;
}
// 关闭socket
void close_socket()
{
	close(connect_socket);
	close(server_socket);
}
// 建立TCP连接并从TCP接收数据
static void tcp_connect(void *pvParameters)
{
	while (1)
	{
		g_rxtx_need_restart = false;
		// 等待WIFI连接信号量，死等
		xEventGroupWaitBits(tcp_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
		ESP_LOGI(TAG, "start tcp connected");
		TaskHandle_t tx_rx_task = NULL;
		TaskHandle_t tx_send_task = NULL;
		//	vTaskDelay(0 / portTICK_RATE_MS); // 延时3S准备建立server
		ESP_LOGI(TAG, "create tcp server");
		int socket_ret = create_tcp_server(true); // 建立server
		if (socket_ret == ESP_FAIL)
		{ // 建立失败
			ESP_LOGI(TAG, "create tcp socket error,stop...");
			continue;
		}
		else
		{ // 建立成功
			ESP_LOGI(TAG, "create tcp socket succeed...");
			// 建立tcp接收数据任务
			if (pdPASS != xTaskCreate(&recv_data, "recv_data", 4096, NULL, 4, &tx_rx_task))
			{
				ESP_LOGI(TAG, "Recv task create fail!");
			}
			else
			{
				xTaskCreate(&send_data, "send_data", 2048, NULL, 4, &tx_send_task);
				ESP_LOGI(TAG, "Recv task create succeed!");
			}
		}
		while (1)
		{
			vTaskDelay(3000 / portTICK_RATE_MS);
			if (g_rxtx_need_restart)
			{ // 重新建立server，流程和上面一样
				ESP_LOGI(TAG, "tcp server error,some client leave,restart...");
				// 建立server
				if (ESP_FAIL != create_tcp_server(false))
				{
					if (pdPASS != xTaskCreate(&recv_data, "recv_data", 4096, NULL, 4, &tx_rx_task))
					{
						ESP_LOGE(TAG, "tcp server Recv task create fail!");
					}
					else
					{
						ESP_LOGI(TAG, "tcp server Recv task create succeed!");
						g_rxtx_need_restart = false; //重新建立完成，清除标记
					}
				}
			}
		}
	}
	vTaskDelete(NULL);
}

// LCD需要初始化一堆命令/参数值，存储在这个结构，看lcd_init函数中while段就明白了
typedef struct
{
	uint8_t cmd;
	uint8_t data[16];
	uint8_t databytes; //数据中没有数据；最高位置1即 bit7 最高位为1会延时后再发送下一组，0xFF是结尾用于循环退出
} lcd_init_cmd_t;

// 液晶初始化参数查PDF文档修改
DRAM_ATTR static const lcd_init_cmd_t ili_init_cmds[] = {
	/* Power contorl B, power control = 0, DC_ENA = 1 */
	{0xCF, {0x00, 0x83, 0X30}, 3},
	/* Power on sequence control,
	 * cp1 keeps 1 frame, 1st frame enable
	 * vcl = 0, ddvdh=3, vgh=1, vgl=2
	 * DDVDH_ENH=1
	 */
	{0xED, {0x64, 0x03, 0X12, 0X81}, 4},
	/* Driver timing control A,
	 * non-overlap=default +1
	 * EQ=default - 1, CR=default
	 * pre-charge=default - 1
	 */
	{0xE8, {0x85, 0x01, 0x79}, 3},
	/* Power control A, Vcore=1.6V, DDVDH=5.6V */
	{0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
	/* Pump ratio control, DDVDH=2xVCl */
	{0xF7, {0x20}, 1},
	/* Driver timing control, all=0 unit */
	{0xEA, {0x00, 0x00}, 2},
	/* Power control 1, GVDD=4.75V */
	{0xC0, {0x26}, 1},
	/* Power control 2, DDVDH=VCl*2, VGH=VCl*7, VGL=-VCl*3 */
	{0xC1, {0x11}, 1},
	/* VCOM control 1, VCOMH=4.025V, VCOML=-0.950V */
	{0xC5, {0x35, 0x3E}, 2},
	/* VCOM control 2, VCOMH=VMH-2, VCOML=VML-2 */
	{0xC7, {0xBE}, 1},
	/* Memory access contorl, MX=MY=0, MV=1, ML=0, BGR=1, MH=0 */
	{0x36, {0x28}, 1},
	/* Pixel format, 16bits/pixel for RGB/MCU interface */
	{0x3A, {0x55}, 1},
	/* Frame rate control, f=fosc, 70Hz fps */
	{0xB1, {0x00, 0x1B}, 2},
	/* Enable 3G, disabled */
	{0xF2, {0x08}, 1},
	/* Gamma set, curve 1 */
	{0x26, {0x01}, 1},
	/* Positive gamma correction */
	{0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0X87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
	/* Negative gamma correction */
	{0XE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
	/* Column address set, SC=0, EC=0xEF */
	{0x2A, {0x00, 0x00, 0x00, 0xEF}, 4},
	/* Page address set, SP=0, EP=0x013F */
	{0x2B, {0x00, 0x00, 0x01, 0x3f}, 4},
	/* Memory write */
	{0x2C, {0}, 0},
	/* Entry mode set, Low vol detect disabled, normal display */
	{0xB7, {0x07}, 1},
	/* Display function control */
	{0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
	/* Sleep out */
	{0x11, {0}, 0x80},
	/* Display on */
	{0x29, {0}, 0x80},
	{0, {0}, 0xff},
};

// 发送命令到LCD，使用轮询方式阻塞等待传输完成。
// 由于数据传输量很少，因此在轮询方式处理可提高速度。使用中断方式的开销要超过轮询方式。
void lcd_cmd(spi_device_handle_t spi, const uint8_t cmd)
{
	esp_err_t ret;
	spi_transaction_t t;
	memset(&t, 0, sizeof(t));					// 清空结构体
	t.length = 8;								// 要传输的位数 一个字节 8位
	t.tx_buffer = &cmd;							// 将命令填充进去
	t.user = (void *)0;							// 设置D/C 线，在SPI传输前回调中根据此值处理DC信号线
	ret = spi_device_polling_transmit(spi, &t); // 开始传输
	assert(ret == ESP_OK);						// 一般不会有问题
}

// 发送数据到LCD，使用轮询方式阻塞等待传输完成。
// 由于数据传输量很少，因此在轮询方式处理可提高速度。使用中断方式的开销要超过轮询方式。
void lcd_data(spi_device_handle_t spi, const uint8_t *data, int len)
{
	esp_err_t ret;
	spi_transaction_t t;
	if (len == 0)
		return;									// 长度为0 没有数据要传输
	memset(&t, 0, sizeof(t));					// 清空结构体
	t.length = len * 8;							// 要写入的数据长度 Len 是字节数，len, transaction length is in bits.
	t.tx_buffer = data;							// 数据指针
	t.user = (void *)1;							// 设置D/C 线，在SPI传输前回调中根据此值处理DC信号线
	ret = spi_device_polling_transmit(spi, &t); // 开始传输
	assert(ret == ESP_OK);						// 一般不会有问题
}

// 此函数在SPI传输开始之前被调用（在irq上下文中！），通过用户字段的值来设置D/C信号线
void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
	int dc = (int)t->user;
	gpio_set_level(PIN_NUM_DC, dc);
}

// 未实现
uint32_t lcd_get_id(spi_device_handle_t spi)
{
	// 获取屏幕驱动芯片ID指令 0x04
	lcd_cmd(spi, 0x04);
	spi_transaction_t t;
	memset(&t, 0, sizeof(t));
	t.length = 8 * 3;
	t.flags = SPI_TRANS_USE_RXDATA;
	t.user = (void *)1;
	esp_err_t ret = spi_device_polling_transmit(spi, &t);
	assert(ret == ESP_OK);
	return *(uint32_t *)t.rx_data;
}

// 初始化SPI液晶屏
// SPI 3.2 240*320 ST7789V / ILI9341
void lcd_init(spi_device_handle_t spi)
{
	int cmd = 0;
	const lcd_init_cmd_t *lcd_init_cmds;

	// 初始化其它控制引脚
	gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT); // 设置D/C 线(cmd/data)

	uint32_t lcd_id = lcd_get_id(spi);

	printf("LCD ID: %08X\n", lcd_id);

	printf("LCD ILI9341 initialization.\n");
	lcd_init_cmds = ili_init_cmds;

	// 循环发送设置所有寄存器
	while (lcd_init_cmds[cmd].databytes != 0xff)
	{
		lcd_cmd(spi, lcd_init_cmds[cmd].cmd);
		lcd_data(spi, lcd_init_cmds[cmd].data, lcd_init_cmds[cmd].databytes & 0x1F);
		if (lcd_init_cmds[cmd].databytes & 0x80)
		{
			vTaskDelay(100 / portTICK_RATE_MS);
		}
		cmd++;
	}

	// gpio_set_level(PIN_NUM_BCKL, 0);	// 点亮LCD屏
}

// 发送一行数据到液晶
static void send_lines(spi_device_handle_t spi, int ypos, uint16_t *linedata)
{
	esp_err_t ret;
	int x;
	// 传输描述符，声明为静态，因此它不是分配在堆栈上；因为在计算下一行的时候，SPI驱动程序还会访问它
	static spi_transaction_t trans[6];
	// 初始化传输
	for (x = 0; x < 6; x++)
	{
		memset(&trans[x], 0, sizeof(spi_transaction_t));
		if ((x & 1) == 0)
		{
			// Even transfers are commands
			trans[x].length = 8;
			trans[x].user = (void *)0;
		}
		else
		{
			// Odd transfers are data
			trans[x].length = 8 * 4;
			trans[x].user = (void *)1;
		}
		trans[x].flags = SPI_TRANS_USE_TXDATA;
	}
	trans[0].tx_data[0] = 0x2A;							  // Column Address Set
	trans[1].tx_data[0] = 0;							  // Start Col High
	trans[1].tx_data[1] = 0;							  // Start Col Low
	trans[1].tx_data[2] = (320) >> 8;					  // End Col High
	trans[1].tx_data[3] = (320) & 0xff;					  // End Col Low
	trans[2].tx_data[0] = 0x2B;							  // Page address set
	trans[3].tx_data[0] = ypos >> 8;					  // Start page high
	trans[3].tx_data[1] = ypos & 0xff;					  // start page low
	trans[3].tx_data[2] = (ypos + PARALLEL_LINES) >> 8;	  // end page high
	trans[3].tx_data[3] = (ypos + PARALLEL_LINES) & 0xff; // end page low
	trans[4].tx_data[0] = 0x2C;							  // memory write
	trans[5].tx_buffer = linedata;						  // finally send the line data
	trans[5].length = 320 * 2 * 8 * PARALLEL_LINES;		  // Data length, in bits
	trans[5].flags = 0;									  // undo SPI_TRANS_USE_TXDATA flag

	// 队列传输所有
	for (x = 0; x < 6; x++)
	{
		ret = spi_device_queue_trans(spi, &trans[x], portMAX_DELAY);
		assert(ret == ESP_OK);
	}
}
static void send_line_finish(spi_device_handle_t spi)
{
	spi_transaction_t *rtrans;
	esp_err_t ret;
	// Wait for all 6 transactions to be done and get back the results.
	for (int x = 0; x < 6; x++)
	{
		ret = spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
		assert(ret == ESP_OK);
		// We could inspect rtrans now if we received any info back. The LCD is treated as write-only, though.
	}
}
void po2(int size)
{
	uint16_t *lines;
	ESP_LOGI(TAG, "jepg  %d", size);
	ESP_ERROR_CHECK(decode_image(&pixels, tcpReceive + size + 10));
	noReceive = 0;
	ReplyFileData(NULL, 0, sendbuff);
	send(connect_socket, sendbuff, 11, 0);
	lines = heap_caps_malloc(320 * PARALLEL_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
	for (int y = 0; y < 240; y += PARALLEL_LINES)
	{
		for (int k = 0; k < 320 * 16; k++)
		{
			lines[k] = pixels[y + k / 320][k % 320];
		}
		send_lines(spi, y, lines);
		send_line_finish(spi);
	}
	heap_caps_free(lines);

}
// 等待传输完成

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
							   int32_t event_id, void *event_data)
{
	if (event_id == WIFI_EVENT_AP_STACONNECTED)
	{
		wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
		ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
				 MAC2STR(event->mac), event->aid);
		xEventGroupSetBits(tcp_event_group, WIFI_CONNECTED_BIT);
	}
	else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
	{
		wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
		ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
				 MAC2STR(event->mac), event->aid);
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED)
	{
		ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip));
		xEventGroupClearBits(tcp_event_group, WIFI_CONNECTED_BIT);
	}
}

void wifi_init_softap(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *wifiAP = esp_netif_create_default_wifi_ap();
	esp_netif_ip_info_t ipInfo;
	IP4_ADDR(&ipInfo.ip, 192, 168, 2, 1);
	IP4_ADDR(&ipInfo.gw, 192, 168, 2, 1);
	IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
	esp_netif_dhcps_stop(wifiAP);
	esp_netif_set_ip_info(wifiAP, &ipInfo);
	esp_netif_dhcps_start(wifiAP);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
														ESP_EVENT_ANY_ID,
														&wifi_event_handler,
														NULL,
														NULL));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL, NULL));

	wifi_config_t wifi_config = {
		.ap = {
			.ssid = EXAMPLE_ESP_WIFI_SSID,
			.ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
			.channel = EXAMPLE_ESP_WIFI_CHANNEL,
			.password = EXAMPLE_ESP_WIFI_PASS,
			.max_connection = EXAMPLE_MAX_STA_CONN,
			.authmode = WIFI_AUTH_WPA_WPA2_PSK},
	};
	if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0)
	{
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
			 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

void app_main(void)
{
	tcp_event_group = xEventGroupCreate();
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// SPI总线配置
	spi_bus_config_t buscfg = {
		.miso_io_num = PIN_NUM_MISO,
		.mosi_io_num = PIN_NUM_MOSI,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = PARALLEL_LINES * 320 * 2 + 8};
	// SPI驱动接口配置
	spi_device_interface_config_t devcfg = {
		.clock_speed_hz = 30 * 1000 * 1000,		 // SPI时钟 30 MHz
		.mode = 0,								 // SPI模式0
		.spics_io_num = PIN_NUM_CS,				 // CS片选信号引脚
		.queue_size = 7,						 // 事务队列大小 7个
		.pre_cb = lcd_spi_pre_transfer_callback, // 数据传输前回调，用作D/C（数据命令）线分别处理
	};
	// 初始化SPI总线
	ret = spi_bus_initialize(LCD_HOST, &buscfg, DMA_CHAN);
	ESP_ERROR_CHECK(ret);
	// 添加SPI总线驱动
	ret = spi_bus_add_device(LCD_HOST, &devcfg, &spi);
	ESP_ERROR_CHECK(ret);
	// 初始化LCD
	lcd_init(spi);
	wifi_init_softap();
	xTaskCreate(&tcp_connect, "tcp_connect", 4096, NULL, 5, NULL);
}
