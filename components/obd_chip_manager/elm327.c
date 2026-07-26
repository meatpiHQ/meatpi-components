/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led.h"
#include "hw_config.h"
#include "debug_logs.h"

/* Defines and Constants */
#define BUF_SIZE 						(1024)
#define ELM327_CMD_QUEUE_SIZE 			100
#define ELM327_MAX_CMD_LEN 				(UART_BUF_SIZE)
#define ELM327_CMD_TIMEOUT_MS   		10000
#define ELM327_CMD_TIMEOUT_US   		(ELM327_CMD_TIMEOUT_MS*1000)  // 10 seconds in microseconds
#define UART_TIMEOUT_MS 				1200
#define DESIRED_BAUD_RATE 				2000000
#define DEFAULT_BAUD_RATE 				115200
// #define DESIRED_BAUD_RATE 115200
// #define DEFAULT_BAUD_RATE 2000000
#define UART_BUFFER_SIZE 				128
#define ELM327_CMD_BUFFER_SIZE 			256
#define ELM327_UPDATE_BUF_SIZE 			512
#define ELM327_UPDATE_MAX_LINE_LENGTH 	256
#define ELM327_UPDATE_TIMEOUT_MS 		5000

#define ELM327_CMD_MUTEX_TIMOUT			10000

typedef struct {
    bool in_normal_state;
    uint16_t device_type;
	bool need_update;
} device_status_t;

typedef struct {
    uint8_t* file_data;
    int32_t file_size;
    uint16_t device_type;
    uint32_t file_lines;
    double file_rawfactor;
    char* device_name;  // Changed to pointer
} firmware_info_t;

static device_status_t device_status = {0};
static firmware_info_t fw_info = {0};
response_callback_t elm327_response;

/* Type Definitions */
typedef struct 
{
    char* command;
    uint32_t command_len;
    QueueHandle_t *response_queue;
    response_callback_t response_callback;
	void *temp;
} elm327_commands_t;

/* Global Variables */
static QueueHandle_t elm327_cmd_queue;
static StaticQueue_t elm327_cmd_queue_struct;
static uint8_t *elm327_cmd_queue_storage = NULL;
QueueHandle_t uart1_queue = NULL;
static SemaphoreHandle_t xuart1_semaphore = NULL;

// extern const unsigned char obd_fw_start[] asm("_binary_V2_3_18_txt_start");
// extern const unsigned char obd_fw_end[]   asm("_binary_V2_3_18_txt_end");
// V2.3.22
extern const unsigned char obd_fw_start[] asm("_binary_V2_3_22_txt_start");
extern const unsigned char obd_fw_end[]   asm("_binary_V2_3_22_txt_end");

static char *update_resp_buf = NULL;

static int uart_read_until_pattern(uart_port_t uart_num, char* buffer, size_t buffer_size, 
                                 const char* end_pattern, int total_timeout_ms) ;

#define ELM327_UART_LOG_BUF_SZ (10*1024)
static char *s_elm_tx_buf = NULL;
static size_t s_elm_tx_len = 0;
static char *s_elm_rx_buf = NULL;
static size_t s_elm_rx_len = 0;
static char* emit_line_buf = NULL;
static bool elm327_udp_log_enabled = false;

static void elm327_trim_trailing_spaces(char *buf, size_t *len)
{
	while (*len > 0 && buf[*len - 1] == ' ')
	{
		(*len)--;
	}
	buf[*len] = 0;
}

static void elm327_emit_line(const char *prefix, const char *payload, bool add_prompt)
{
	if (!elm327_udp_log_enabled)
	{
		return;
	}

	char *out = emit_line_buf;
	if (!out)
	{
		return;
	}
	if (!payload)
	{
		payload = "";
	}
	const size_t cap = (size_t)ELM327_UART_LOG_BUF_SZ;
	out[0] = 0;
	(void)strlcpy(out, prefix ? prefix : "", cap);
	(void)strlcat(out, payload, cap);
	if (add_prompt)
	{
		(void)strlcat(out, (payload[0] ? " >" : ">"), cap);
	}
	debug_logs_log(DEBUG_LOG_LEVEL_INFO, "ELM327_UART", "%s", out);
}

static void elm327_uart_log_tx_bytes(uart_port_t uart_num, const uint8_t *data, int len)
{
	(void)uart_num;
	if (!data || len <= 0)
	{
		return;
	}
	if (!s_elm_tx_buf)
	{
		return;
	}
	for (int i = 0; i < len; i++)
	{
		uint8_t c = data[i];
		if (c == '\r')
		{
			s_elm_tx_buf[s_elm_tx_len] = 0;
			elm327_trim_trailing_spaces(s_elm_tx_buf, &s_elm_tx_len);
			if (s_elm_tx_len > 0)
			{
				elm327_emit_line("ELM TX: ", s_elm_tx_buf, false);
			}
			s_elm_tx_len = 0;
			continue;
		}
		if (c == '\n')
		{
			continue;
		}
		if (c == '\t')
		{
			c = ' ';
		}
		if (c < 32 || c > 126)
		{
			c = '?';
		}

		if (s_elm_tx_len + 1 >= ELM327_UART_LOG_BUF_SZ)
		{
			s_elm_tx_buf[s_elm_tx_len] = 0;
			elm327_trim_trailing_spaces(s_elm_tx_buf, &s_elm_tx_len);
			if (s_elm_tx_len > 0)
			{
				elm327_emit_line("ELM TX: ", s_elm_tx_buf, false);
			}
			s_elm_tx_len = 0;
		}
		s_elm_tx_buf[s_elm_tx_len++] = (char)c;
		s_elm_tx_buf[s_elm_tx_len] = 0;
	}
}

static void elm327_uart_log_rx_bytes(uart_port_t uart_num, const uint8_t *data, int len)
{
	(void)uart_num;
	if (!data || len <= 0)
	{
		return;
	}
	if (!s_elm_rx_buf)
	{
		return;
	}
	for (int i = 0; i < len; i++)
	{
		uint8_t c = data[i];
		if (c == '>')
		{
			s_elm_rx_buf[s_elm_rx_len] = 0;
			elm327_trim_trailing_spaces(s_elm_rx_buf, &s_elm_rx_len);
			if (s_elm_rx_len > 0)
			{
				elm327_emit_line("ELM RX: ", s_elm_rx_buf, false);
			}
			elm327_emit_line("ELM RX: ", "", true);
			s_elm_rx_len = 0;
			continue;
		}
		if (c == '\r' || c == '\n' || c == '\t')
		{
			// Treat line breaks as a single space separator
			if (s_elm_rx_len > 0 && s_elm_rx_buf[s_elm_rx_len - 1] != ' ')
			{
				if (s_elm_rx_len + 1 < ELM327_UART_LOG_BUF_SZ)
				{
					s_elm_rx_buf[s_elm_rx_len++] = ' ';
					s_elm_rx_buf[s_elm_rx_len] = 0;
				}
			}
			continue;
		}
		if (c < 32 || c > 126)
		{
			c = '?';
		}

		if (s_elm_rx_len + 1 >= ELM327_UART_LOG_BUF_SZ)
		{
			s_elm_rx_buf[s_elm_rx_len] = 0;
			elm327_trim_trailing_spaces(s_elm_rx_buf, &s_elm_rx_len);
			if (s_elm_rx_len > 0)
			{
				elm327_emit_line("ELM RX: ", s_elm_rx_buf, false);
			}
			s_elm_rx_len = 0;
		}
		s_elm_rx_buf[s_elm_rx_len++] = (char)c;
		s_elm_rx_buf[s_elm_rx_len] = 0;
	}
}

static int elm327_uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
	int written = uart_write_bytes(uart_num, src, size);
	if(elm327_udp_log_enabled)	//if log to udp enabled place holder here
	{
		if (written > 0 && src)
		{
			elm327_uart_log_tx_bytes(uart_num, (const uint8_t*)src, written);
		}
	}
	return written;
}

static int elm327_uart_read_bytes(uart_port_t uart_num, void *buf, size_t size, TickType_t ticks_to_wait)
{
	int r = uart_read_bytes(uart_num, buf, size, ticks_to_wait);
	if(elm327_udp_log_enabled)	//if log to udp enabled place holder here
	{
		if (r > 0 && buf)
		{
			elm327_uart_log_rx_bytes(uart_num, (const uint8_t*)buf, r);
		}
	} 
	return r;
}

static void elm327_powerpin_commands(void)
{
	bool hardreset_needed = 0;

	ESP_LOGI(TAG, "Setting PPSW to 10");
	if(xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		char* rx_buffer = (char*)heap_caps_malloc(ELM327_CMD_BUFFER_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		if(rx_buffer == NULL)
		{
			ESP_LOGE(TAG, "Failed to allocate memory for rx_buffer");
			xSemaphoreGive(xuart1_semaphore);
			return;
		}
		bzero(rx_buffer, ELM327_CMD_BUFFER_SIZE);
		uart_flush_input(UART_NUM_1);

		// VTVERS
		elm327_uart_write_bytes(UART_NUM_1, "VTVERS\r", strlen("VTVERS\r"));
		uart_read_until_pattern(UART_NUM_1, rx_buffer, ELM327_CMD_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
		if (strstr(rx_buffer, OBD_FW_VER_V22) == NULL)
		{
			xSemaphoreGive(xuart1_semaphore);
			free(rx_buffer);
			return;
		}

		elm327_uart_write_bytes(UART_NUM_1, "VTPPSWS\r", strlen("VTPPSWS\r"));
		uart_read_until_pattern(UART_NUM_1, rx_buffer, ELM327_CMD_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
		if(strstr(rx_buffer, "10") != NULL)
		{
			ESP_LOGI(TAG, "PPSW is already 10");
		}
		else
		{
			ESP_LOGW(TAG, "PPSW is not 10, setting to 10");
			elm327_uart_write_bytes(UART_NUM_1, "VTPPSW10\r", strlen("VTPPSW10\r"));
			uart_read_until_pattern(UART_NUM_1, rx_buffer, ELM327_CMD_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
			if (strstr(rx_buffer, "OK") != NULL)
			{
				ESP_LOGI(TAG, "PPSW set to 10 successfully");
			}
			else
			{
				ESP_LOGE(TAG, "Failed to set PPSW to 10");
			}
			
			hardreset_needed = true;
		}
		
		xSemaphoreGive(xuart1_semaphore);
		free(rx_buffer);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take UART semaphore", __func__);
	}
	if(hardreset_needed)
	{
		vTaskDelay(pdMS_TO_TICKS(100));
		elm327_hardreset_chip();
	}
}

/* Function Implementations */
int8_t elm327_process_cmd(uint8_t *cmd, uint32_t len, QueueHandle_t *q, 
                         char *cmd_buffer, uint32_t *cmd_buffer_len, 
                         int64_t *last_cmd_time, response_callback_t response_callback)
{
    int64_t current_time = esp_timer_get_time();

    if(len == 0)
    {
        len = strlen((char*)cmd);
    }

    if (current_time - *last_cmd_time > ELM327_CMD_TIMEOUT_US)
    {
        ESP_LOGW(TAG, "Timeout occurred, resetting command buffer.");
        *cmd_buffer_len = 0;
		cmd_buffer[0] = '\0';
    }

    *last_cmd_time = current_time;

    for (int i = 0; i < len; i++)
    {
        if (*cmd_buffer_len < ELM327_MAX_CMD_LEN - 1)
        {
            cmd_buffer[(*cmd_buffer_len)++] = cmd[i];
			cmd_buffer[*cmd_buffer_len] = '\0';
        }

        if (cmd[i] == '\r')
        {
            elm327_commands_t *command_data;
			// command_data = (elm327_commands_t*) malloc(sizeof(elm327_commands_t));
			command_data = (elm327_commands_t*) heap_caps_aligned_alloc(16, sizeof(elm327_commands_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (command_data == NULL)
			{
				ESP_LOGE(TAG, "Failed to allocate memory for command_data");
				*cmd_buffer_len = 0;
				cmd_buffer[0] = '\0';
				return -1;
			}
			memset(command_data, 0, sizeof(elm327_commands_t));
            // command_data->command = (char*) malloc(*cmd_buffer_len + 1);
			command_data->command = (char*) heap_caps_aligned_alloc(16, *cmd_buffer_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (command_data->command == NULL)
            {
                ESP_LOGE(TAG, "Failed to allocate memory for command");
				free(command_data);
                *cmd_buffer_len = 0;
				cmd_buffer[0] = '\0';
                return -1;
            }
			memset(command_data->command,0,*cmd_buffer_len + 1);
			memcpy(command_data->command, cmd_buffer, *cmd_buffer_len);
			command_data->command[*cmd_buffer_len] = '\0';
            command_data->command_len = *cmd_buffer_len;
            command_data->response_queue = q;
            command_data->response_callback = response_callback;
			command_data->temp = command_data;
            if (xQueueSend(elm327_cmd_queue, (void*)command_data, portMAX_DELAY) != pdPASS)
            {
                ESP_LOGE(TAG, "Failed to send command to the queue");
                free(command_data->command);
				free(command_data);
                *cmd_buffer_len = 0;
				cmd_buffer[0] = '\0';
                return -1;
            }

            *cmd_buffer_len = 0;
			cmd_buffer[0] = '\0';
        }
    }

    return 0;
}

void elm327_send_cmd(uint8_t* cmd, uint32_t cmd_len)
{
	if(xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		ESP_LOG_BUFFER_HEXDUMP(TAG, (char*)cmd, cmd_len, ESP_LOG_INFO);

		// Support multiple commands in one buffer: process per CR/CRLF-delimited segment
		#define IS_WS(c) ((c) == ' ' || (c) == '\t')
		#define EQLI(a,b) (((a) | 0x20) == ((b) | 0x20))
		uint32_t pos = 0;
		while (pos < cmd_len) {
			// locate end of segment (delimiter is \r or \n)
			uint32_t seg_start = pos;
			uint32_t seg_end = pos;
			while (seg_end < cmd_len && cmd[seg_end] != '\r' && cmd[seg_end] != '\n') { seg_end++; }
			// analyze segment [seg_start, seg_end)
			int atz_seg = 0;
			uint32_t i = seg_start;
			while (i < seg_end && IS_WS(cmd[i])) { i++; }
			if (i < seg_end && EQLI(cmd[i], 'a')) {
				i++;
				while (i < seg_end && IS_WS(cmd[i])) { i++; }
				if (i < seg_end && EQLI(cmd[i], 't')) {
					i++;
					while (i < seg_end && IS_WS(cmd[i])) { i++; }
					if (i < seg_end && EQLI(cmd[i], 'z')) {
						i++;
						while (i < seg_end && IS_WS(cmd[i])) { i++; }
						if (i == seg_end) { atz_seg = 1; }
					}
				}
			}

			// determine delimiter length to consume (\r? then optional \n)
			uint32_t delim_len = 0;
			if (seg_end < cmd_len) {
				if (cmd[seg_end] == '\r') {
					delim_len++;
					if (seg_end + delim_len < cmd_len && cmd[seg_end + delim_len] == '\n') { delim_len++; }
				} else if (cmd[seg_end] == '\n') {
					delim_len++;
				}
			}

			if (atz_seg) {
				static const char replace_cmd[] = "ATWS\r";
				ESP_LOGI(TAG, "Replaced ATZ command with ATWS");
				elm327_uart_write_bytes(UART_NUM_1, (const uint8_t*)replace_cmd, sizeof(replace_cmd) - 1);
			} else {
				// write original segment including its delimiters
				if (seg_start < seg_end) {
					elm327_uart_write_bytes(UART_NUM_1, (const uint8_t*)&cmd[seg_start], seg_end - seg_start);
				}
				if (delim_len) {
					elm327_uart_write_bytes(UART_NUM_1, (const uint8_t*)&cmd[seg_end], delim_len);
				}
			}

			pos = seg_end + delim_len;
			if (seg_end == cmd_len) { break; }
		}
		#undef IS_WS
		#undef EQLI
		
		xSemaphoreGive(xuart1_semaphore);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take UART semaphore", __func__);
	}
}

static void uart1_event_task(void *pvParameters)
{
    static uint8_t *uart_read_buf __attribute__((aligned(4))) DRAM_ATTR;
    static DRAM_ATTR elm327_commands_t elm327_command;
    size_t response_len = 0;
    uart_event_t event;

	uart_read_buf = (uint8_t *)heap_caps_aligned_alloc(4, ELM327_MAX_CMD_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!uart_read_buf)
	{
		ESP_LOGE(TAG, "Failed to allocate uart_read_buf");
		vTaskDelete(NULL);
		return;
	}
	memset(uart_read_buf, 0, ELM327_MAX_CMD_LEN);

    while (1) 
    {
		dev_status_wait_for_bits(DEV_AWAKE_BIT, portMAX_DELAY);
        if (xQueueReceive(elm327_cmd_queue, (void*)&elm327_command, portMAX_DELAY) == pdTRUE)
        {
			sleep_state_info_t sleep_state;
			sleep_mode_get_state(&sleep_state);
			memset(uart_read_buf, 0, ELM327_MAX_CMD_LEN);
            if ((elm327_chip_get_status() == ELM327_READY) && (sleep_state.state != STATE_SLEEPING) && xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
            {
                // uart_flush(UART_NUM_1);
                uart_event_t event;
                static const char atze_fake_rsp[] = "ATZ\r\r\rELM327 v2.3\r\r>";
				static const char atz_fake_rsp[] = "\r\rELM327 v2.3\r\r>";
				uint8_t atz_flag = 0;
				esp_err_t tx_wait_ret = ESP_FAIL;

                // while (xQueueReceive(uart1_queue, &event, 0) == pdTRUE) 
                // {
                //     ESP_LOGW(TAG, "Discarding UART event: %d", event.type);
				// 	elm327_uart_read_bytes(UART_NUM_1, uart_read_buf, sizeof(uart_read_buf), 0);
                // }
                
                // elm327_uart_read_bytes(UART_NUM_1, uart_read_buf, sizeof(uart_read_buf), 0);
				uart_flush_input(UART_NUM_1);
				xQueueReset(uart1_queue);

                if(strstr(elm327_command.command, "ATZ\r") != NULL || 
                   strstr(elm327_command.command, "atz\r") != NULL ||
                   strstr(elm327_command.command, "AT Z\r") != NULL ||
                   strstr(elm327_command.command, "at z\r") != NULL)
                {
					// if(strlen(elm327_command.command) < sizeof(last_command))
					// {
					// 	strcpy(last_command, elm327_command.command);
					// }
                    elm327_uart_write_bytes(UART_NUM_1, "ATWS\r", strlen("ATWS\r"));
					tx_wait_ret = uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
					if(tx_wait_ret != ESP_OK )
					{
						ESP_LOGE(TAG, "uart_wait_tx_done returned error");
					}
					atz_flag = 1;
                    ESP_LOGI(TAG, "Replaced ATZ with ATWS command");
                }
				// else if(elm327_command.command[0] == '\r' && elm327_command.command[1] == 0)
				// {
				// 	elm327_uart_write_bytes(UART_NUM_1, last_command, strlen(last_command));
				// 	ESP_LOGI(TAG, "Repeat last command");
				// 	printf("Repeat last command: \r\n%s\r\n", last_command);
				// 	printf("-----\r\n");
				// 	ESP_LOGW(TAG, "-------------Sent");
				// 	ESP_LOG_BUFFER_HEXDUMP(TAG, last_command, strlen(last_command), ESP_LOG_INFO);
				// 	ESP_LOGW(TAG, "-------------Sent");
				// }
                else 
                {
					// if(strlen(elm327_command.command) < sizeof(last_command))
					// {
					// 	strcpy(last_command, elm327_command.command);
					// }
                    elm327_uart_write_bytes(UART_NUM_1, elm327_command.command, elm327_command.command_len);
					tx_wait_ret = uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
					if(tx_wait_ret != ESP_OK )
					{
						ESP_LOGE(TAG, "uart_wait_tx_done returned error");
					}
					ESP_LOGW(TAG, "-------------Sent");
					ESP_LOG_BUFFER_HEXDUMP(TAG, elm327_command.command, elm327_command.command_len, ESP_LOG_INFO);
					ESP_LOGW(TAG, "-------------Sent");
                }

                bool terminator_received = false;
                response_len = 0;

                while (!terminator_received)
                {
                    if (xQueueReceive(uart1_queue, (void*)&event, pdMS_TO_TICKS(ELM327_CMD_TIMEOUT_MS)) == pdTRUE)
                    {
                        if (event.type == UART_DATA)
                        {
                            int read_bytes = elm327_uart_read_bytes(UART_NUM_1, 
                                                           uart_read_buf+response_len, 
                                                           event.size, 
                                                           pdMS_TO_TICKS(1));
                            if (read_bytes > 0)
                            {
								ESP_LOGW(TAG, "-------------Received");
                                ESP_LOG_BUFFER_HEXDUMP(TAG, uart_read_buf + response_len, 
                                                     read_bytes, ESP_LOG_INFO);
								ESP_LOGW(TAG, "-------------Received");
                                response_len += read_bytes;
								uart_read_buf[response_len] = 0;
								// elm327_command.response_callback((char*)uart_read_buf, 
								// 									read_bytes, 
								// 									elm327_command.response_queue, 
								// 									elm327_command.command);
								for (int i = 0; i < read_bytes; i++)
                                {
									if(uart_read_buf[i] == 0xA5)
									{
										printf("A5 detected\r\n");
									}
								}
                                for (int i = 0; i < response_len+1; i++)
                                {
                                    // if ((i > 1 && uart_read_buf[i-1] == '\r' && uart_read_buf[i] == '>') || 
                                    //     (i > 2 && uart_read_buf[i-2] == 'O' && uart_read_buf[i-1] == 'K' && 
                                    //      uart_read_buf[i] == '\r'))
									
									if ((i >= 2 && uart_read_buf[i-2] == '\r' && uart_read_buf[i-1] == '>' && uart_read_buf[i] == '\0'))
                                    {
                                        // if(strstr((char*)uart_read_buf, "STSBR2000000\rOK"))
                                        // {
                                        //     elm327_uart_write_bytes(UART_NUM_1, "\r\r", 2);
                                        //     elm327_uart_write_bytes(UART_NUM_1, "STWBR\r", strlen("STWBR\r"));
                                        // }
										ESP_LOGI(TAG, "Terminator Received");
                                        terminator_received = true;
                                        break;
                                    }
									// else if(uart_read_buf[response_len-1] == '\r')
									// {
									// 	elm327_command.response_callback((char*)uart_read_buf, 
									// 										response_len, 
									// 										elm327_command.response_queue, 
									// 										elm327_command.command);
									// 	response_len = 0;
									// }
                                }
                            }
                        }
                    }
                    else
                    {
                        ESP_LOGE(TAG, "UART read timeout");
                        break;
                    }
                }

                if (terminator_received && response_len > 0)
                {
                    uart_read_buf[response_len] = '\0';

                    if (elm327_command.response_callback != NULL)
                    {
						if(atz_flag == 1)
						{
							atz_flag = 0;
							if(strstr((char*)uart_read_buf, "WS") != NULL || strstr((char*)uart_read_buf, "ws") != NULL)
							{
								elm327_command.response_callback(atze_fake_rsp, strlen(atze_fake_rsp), 
															elm327_command.response_queue, 
															elm327_command.command);
							}
							else
							{
								elm327_command.response_callback(atz_fake_rsp, strlen(atz_fake_rsp), 
															elm327_command.response_queue, 
															elm327_command.command);
							}
						}
                        else
						{
							elm327_command.response_callback((char*)uart_read_buf, 
                                                       response_len, 
                                                       elm327_command.response_queue, 
                                                       elm327_command.command);
						}
                    }
                }

                xSemaphoreGive(xuart1_semaphore);
            }

            free(elm327_command.command);
			free(elm327_command.temp);
        }
    }
}

#define READ_TIMEOUT_MS 10
#define MAX_TOTAL_TIMEOUT_MS 1000

static int uart_read_until_pattern(uart_port_t uart_num, char* buffer, size_t buffer_size, 
                                 const char* end_pattern, int total_timeout_ms) 
{
    int total_len = 0;
    int64_t start_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
    
    while (total_len < buffer_size - 1) 
	{  // Leave space for null terminator
        // Read a small chunk
        int len = elm327_uart_read_bytes(uart_num, buffer + total_len, 
                                buffer_size - total_len - 1, READ_TIMEOUT_MS);
        
        if (len > 0)
		{
            total_len += len;
            buffer[total_len] = '\0';  // Null terminate for string operations
            // ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, total_len, ESP_LOG_INFO);
            // Check if we found our pattern
            if (strstr(buffer, end_pattern))
			{
                return total_len;
            }
        }
        
        // Check if we've exceeded our total timeout
        if ((esp_timer_get_time() / 1000 - start_time) >= total_timeout_ms)
		{
			ESP_LOGE(TAG, "Timeout!");
            break;
        }
    }
    
    return total_len;
}

bool elm327_set_baudrate(void)
{
    char rx_buffer[UART_BUFFER_SIZE];
    int len;
    bool success = false;
    char command[20];
    
    if (xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
    {
        uart_flush(UART_NUM_1);
        if (uart1_queue)
        {
            xQueueReset(uart1_queue);
        }
        
        // uart_set_baudrate(UART_NUM_1, DESIRED_BAUD_RATE);
        ESP_LOGI(TAG, "Trying %d baud", DESIRED_BAUD_RATE);

        elm327_uart_write_bytes(UART_NUM_1, "VTVERS\r", 7);
        
        len = uart_read_until_pattern(UART_NUM_1, rx_buffer, UART_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
        
        if (len > 2 && rx_buffer[len-2] == '\r' && rx_buffer[len-1] == '>')
        {
            ESP_LOGI(TAG, "Device already at %d baud", DESIRED_BAUD_RATE);
            success = true;
        }
        else
        {
            uart_set_baudrate(UART_NUM_1, DEFAULT_BAUD_RATE);
            ESP_LOGI(TAG, "Trying %d baud", DEFAULT_BAUD_RATE);
            
            elm327_uart_write_bytes(UART_NUM_1, "VTVERS\r", 7);
            len = uart_read_until_pattern(UART_NUM_1, rx_buffer, UART_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
            
            if (len > 2 && rx_buffer[len-2] == '\r' && rx_buffer[len-1] == '>')
            {
                ESP_LOGI(TAG, "Connected at %d baud, switching to %d", 
                        DEFAULT_BAUD_RATE, DESIRED_BAUD_RATE);
                
                snprintf(command, sizeof(command), "STSBR %d\r", DESIRED_BAUD_RATE);
                elm327_uart_write_bytes(UART_NUM_1, command, strlen(command));
                len = uart_read_until_pattern(UART_NUM_1, rx_buffer, UART_BUFFER_SIZE - 1, "OK", UART_TIMEOUT_MS);
                
                if (len > 0 && strstr(rx_buffer, "OK"))
                {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    uart_set_baudrate(UART_NUM_1, DESIRED_BAUD_RATE);
                    
                    elm327_uart_write_bytes(UART_NUM_1, "VTVERS\r", 7);
                    len = uart_read_until_pattern(UART_NUM_1, rx_buffer, UART_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
                    
                    if (len > 2 && rx_buffer[len-2] == '\r' && rx_buffer[len-1] == '>')
                    {
                        ESP_LOGI(TAG, "Successfully switched to %d baud", DESIRED_BAUD_RATE);
                        success = true;
                        
                        elm327_uart_write_bytes(UART_NUM_1, "STWBR\r", 6);
                        uart_read_until_pattern(UART_NUM_1, rx_buffer, UART_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Failed to verify new baud rate");
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to change baud rate");
                }
            }
            else
            {
                ESP_LOGE(TAG, "No response at %d baud", DEFAULT_BAUD_RATE);
            }
        }
		xSemaphoreGive(xuart1_semaphore);
    }
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take UART semaphore", __func__);
	}
    
    return success;
}

void elm327_lock(void)
{
	xSemaphoreTake(xuart1_semaphore, portMAX_DELAY);
}

void elm327_hardreset_chip(void)
{
    char *rsp_buffer = (char *)heap_caps_malloc(UART_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t rsp_len;
	ESP_LOGW(TAG, "Performing hard reset of ELM327 chip");
	if (xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		vTaskDelay(pdMS_TO_TICKS(500));
		uart_flush_input(UART_NUM_1);
		// xQueueReset(uart1_queue);
		if(gpio_get_level(OBD_READY_PIN) == 1)
		{
			ESP_LOGW(TAG, "OBD_READY_PIN is high, performing hardware reset");
			gpio_set_level(OBD_RESET_PIN, 0);
			vTaskDelay(pdMS_TO_TICKS(5));
			gpio_set_level(OBD_RESET_PIN, 1);
		}
		else
		{
			ESP_LOGI(TAG, "OBD_READY_PIN is low, sending ATZ command instead of hardware reset");
			elm327_uart_write_bytes(UART_NUM_1, "ATZ\r", strlen("ATZ\r"));
		}
		memset(rsp_buffer, 0, UART_BUFFER_SIZE);
        int len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, UART_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS+300);
		if(len > 0)
		{
			// ESP_LOG_BUFFER_CHAR(TAG, rsp_buffer, len);
			ESP_LOGW(TAG, "Hardreset OK");
		}
		else
		{
			ESP_LOGE(TAG, "Hardreset failed");
		}
		uart_flush_input(UART_NUM_1);
		if(xuart1_semaphore == NULL)
		{
			ESP_LOGE(TAG, "xuart1_semaphore is NULL");
		}
		xSemaphoreGive(xuart1_semaphore);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take UART semaphore", __func__);
	}

	vTaskDelay(pdMS_TO_TICKS(50));
    if (elm327_set_baudrate())
    {
        ESP_LOGI(TAG, "UART configuration completed successfully");
    }
    else
    {
        ESP_LOGE(TAG, "UART configuration failed");
    }
	// uint8_t protocol_number = 0;
	// elm327_get_protocol_number(&protocol_number);
}

esp_err_t elm327_get_protocol_number(uint8_t *protocol_number)
{
	esp_err_t ret = ESP_FAIL;
	char *rsp_buffer = NULL;
	uint32_t rsp_len;

	if (protocol_number == NULL) {
		ESP_LOGE(TAG, "Invalid protocol_number pointer");
		return ESP_ERR_INVALID_ARG;
	}

	rsp_buffer = (char *) heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (rsp_buffer == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	
	if (xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);

		//Step 1: Send ATZ to reset device
		elm327_uart_write_bytes(UART_NUM_1, "ATWS\r", strlen("ATWS\r"));
		int len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, 512, "\r>", UART_TIMEOUT_MS+300);
		
		if (len > 0) {
			ESP_LOGI(TAG, "ATWS response:");
			ESP_LOG_BUFFER_HEXDUMP(TAG, rsp_buffer, len, ESP_LOG_INFO);
			if (strstr(rsp_buffer, "ELM327") == NULL) {
				ESP_LOGW(TAG, "ATWS did not return ELM327, continuing anyway");
			}
		} else {
			ESP_LOGE(TAG, "No response to ATWS command");
			ret = ESP_ERR_TIMEOUT;
			goto cleanup;
		}
		// Step 2: Set automatic protocol with ATTP0
		elm327_uart_write_bytes(UART_NUM_1, "ATTP0\r", strlen("ATTP0\r"));
		len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, 512, "\r>", UART_TIMEOUT_MS+300);
		
		if (len > 0) {
			ESP_LOGI(TAG, "ATTP0 response:");
			ESP_LOG_BUFFER_HEXDUMP(TAG, rsp_buffer, len, ESP_LOG_INFO);
			if (strstr(rsp_buffer, "OK") == NULL) {
				ESP_LOGW(TAG, "ATTP0 did not return OK, continuing anyway");
			}
		} else {
			ESP_LOGE(TAG, "No response to ATTP0 command");
			ret = ESP_ERR_TIMEOUT;
			goto cleanup;
		}
		
		// Clear buffer for next command
		memset(rsp_buffer, 0, 512);
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);
		
		// Step 2: Send 0100 to establish protocol
		elm327_uart_write_bytes(UART_NUM_1, "0100\r", strlen("0100\r"));
		len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, 512, "\r>", UART_TIMEOUT_MS+10000);
		
		if (len > 0) {
			ESP_LOGI(TAG, "0100 response:");
			ESP_LOG_BUFFER_HEXDUMP(TAG, rsp_buffer, len, ESP_LOG_INFO);
			// Check if we got a valid response (not "NO DATA" or "BUS INIT ERROR")
			if (strstr(rsp_buffer, "NO DATA") != NULL || 
				strstr(rsp_buffer, "BUS INIT") != NULL ||
				strstr(rsp_buffer, "ERROR") != NULL) {
				ESP_LOGE(TAG, "Failed to establish protocol with 0100 command");
				ret = ESP_ERR_NOT_FOUND;
				goto cleanup;
			}
		} else {
			ESP_LOGE(TAG, "No response to 0100 command");
			ret = ESP_ERR_TIMEOUT;
			goto cleanup;
		}
		
		// Clear buffer for final command
		memset(rsp_buffer, 0, 512);
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);
		
		// Step 3: Get protocol number with ATDPN
        elm327_uart_write_bytes(UART_NUM_1, "ATDPN\r", strlen("ATDPN\r"));
        len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, 512, "\r>", UART_TIMEOUT_MS+300);
		
		if (len > 0) {
			ESP_LOGI(TAG, "ATDPN response:");
			ESP_LOG_BUFFER_HEXDUMP(TAG, rsp_buffer, len, ESP_LOG_INFO);
			// Parse the response - look for protocol number
			char *protocol_start = NULL;
			uint8_t skip_char = 0;
			if(strstr(rsp_buffer, "ATDPN\r") != NULL){
				ESP_LOGI(TAG, "Echo is enabled");
				skip_char = strlen("ATDPN\r");
			}

			// Check for "AX" format first
			if (len >= 2 && rsp_buffer[0] == 'A') {
				protocol_start = &rsp_buffer[1 + skip_char];
			}
			// Check for "X" format
			else if (len >= 1) {
				protocol_start = &rsp_buffer[0 + skip_char];
			}
			
			if (protocol_start != NULL) {
				char protocol_char = protocol_start[0];
				
				// Convert hex character to number (0-9, A-C)
				if (protocol_char >= '0' && protocol_char <= '9') {
					*protocol_number = protocol_char - '0';
					ret = ESP_OK;
				}
				else if (protocol_char >= 'A' && protocol_char <= 'C') {
					*protocol_number = protocol_char - 'A' + 10;
					ret = ESP_OK;
				}
				else if (protocol_char >= 'a' && protocol_char <= 'c') {
					*protocol_number = protocol_char - 'a' + 10;
					ret = ESP_OK;
				}
				else {
					ESP_LOGE(TAG, "Invalid protocol character: %c", protocol_char);
					ret = ESP_ERR_INVALID_RESPONSE;
				}
				
				if (ret == ESP_OK) {
					ESP_LOGI(TAG, "Protocol number: %d", *protocol_number);
				}
			}
			else {
				ESP_LOGE(TAG, "Could not parse protocol from response");
				ret = ESP_ERR_INVALID_RESPONSE;
			}
		}
		else {
			ESP_LOGE(TAG, "No response received or timeout");
			ret = ESP_ERR_TIMEOUT;
		}
		
cleanup:
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);
		xSemaphoreGive(xuart1_semaphore);
	}
	else {
		ESP_LOGE(TAG, "%s: Failed to take semaphore", __func__);
		ret = ESP_ERR_TIMEOUT;
	}
	
	// Free allocated memory
	if (rsp_buffer != NULL) {
		heap_caps_free(rsp_buffer);
	}
	
	return ret;
}

esp_err_t elm327_sleep(void)
{
    static char rsp_buffer[100];
    uint32_t rsp_len;
	esp_err_t ret = ESP_FAIL;

	if (xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		ESP_LOGI(TAG, "Got semaphore, preparing to sleep");
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);
        elm327_uart_write_bytes(UART_NUM_1, "STSLEEP0\r", strlen("STSLEEP0\r"));
        int len = uart_read_until_pattern(UART_NUM_1, rsp_buffer, sizeof(rsp_buffer), "\r>", UART_TIMEOUT_MS+300);
		if(len > 0 && strstr(rsp_buffer, "OK\r\r>"))
		{
			printf("Sleep OK\r\n");
			ESP_LOGW(TAG, "Sleep OK");
			ret = ESP_OK;
		}
		else
		{
			ESP_LOGE(TAG, "%s: Sleep failed", __func__);
			ret = ESP_FAIL;
		}
		uart_flush_input(UART_NUM_1);

		gpio_sleep_set_pull_mode(OBD_SLEEP_PIN, GPIO_PULLDOWN_ONLY);
		gpio_set_level(OBD_SLEEP_PIN, 0);
		gpio_pulldown_en(OBD_SLEEP_PIN);
		rtc_gpio_pulldown_en(OBD_SLEEP_PIN);
		gpio_hold_en(OBD_SLEEP_PIN);

		
		gpio_sleep_set_pull_mode(OBD_READY_PIN, GPIO_PULLDOWN_ONLY);
		rtc_gpio_pulldown_en(OBD_READY_PIN);
		gpio_pulldown_en(OBD_READY_PIN);
		gpio_hold_en(OBD_READY_PIN);
		gpio_deep_sleep_hold_en();

		// xQueueReset(uart1_queue);
		xSemaphoreGive(xuart1_semaphore);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take semaphore", __func__);
	}
	return ret;
}

static bool is_hex_char(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static bool is_likely_can_frame_line(const char *line, size_t len)
{
	if (!line || len == 0)
		return false;

	// Trim leading whitespace
	size_t s = 0;
	while (s < len && (line[s] == ' ' || line[s] == '\t'))
		s++;
	if (s >= len)
		return false;

	// Find first space separating header and data
	size_t sp = s;
	while (sp < len && line[sp] != ' ' && line[sp] != '\t')
		sp++;
	if (sp <= s || sp >= len)
		return false;

	// Header length is typically 3 (11-bit) or 8 (29-bit), but allow a bit of slack.
	size_t header_len = sp - s;
	if (header_len < 2 || header_len > 8)
		return false;
	for (size_t i = s; i < sp; i++)
	{
		if (!is_hex_char(line[i]))
			return false;
	}

	// Skip whitespace after header
	size_t d = sp;
	while (d < len && (line[d] == ' ' || line[d] == '\t'))
		d++;
	if (d + 1 >= len)
		return false;

	// Need at least one data byte (two hex chars)
	return is_hex_char(line[d]) && is_hex_char(line[d + 1]);
}

static bool elm327_scan_for_prompt(const uint8_t *buf, int len, bool *last_was_cr)
{
	if (!buf || len <= 0 || !last_was_cr)
		return false;
	for (int i = 0; i < len; i++)
	{
		uint8_t b = buf[i];
		if (*last_was_cr && b == (uint8_t)'>')
			return true;
		*last_was_cr = (b == (uint8_t)'\r');
	}
	return false;
}

static bool elm327_is_hex_n(const char *s, size_t n)
{
	if (!s || n == 0)
		return false;
	for (size_t i = 0; i < n; i++)
	{
		char c = s[i];
		bool is_hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
		if (!is_hex)
			return false;
	}
	return true;
}

static bool elm327_atma_line_matches_frame_id(const char *line, size_t line_len, uint32_t frame_id)
{
	if (!line || line_len == 0)
		return false;
	if (frame_id == 0)
		return true;

	// Trim leading spaces
	while (line_len > 0 && (*line == ' ' || *line == '\t'))
	{
		line++;
		line_len--;
	}
	// Trim trailing spaces
	while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t'))
		line_len--;
	if (line_len == 0)
		return false;

	const char *p = line;
	const char *end = line + line_len;

	// token 1
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	const char *t1 = p;
	while (p < end && *p != ' ' && *p != '\t')
		p++;
	size_t t1_len = (size_t)(p - t1);
	if (t1_len == 0)
		return false;

	// Case A: contiguous header in token1: 2..8 hex chars ("FF", "123", "000008C0")
	if (t1_len >= 2 && t1_len <= 8 && elm327_is_hex_n(t1, t1_len))
	{
		char tmp[9] = {0};
		memcpy(tmp, t1, t1_len);
		uint32_t hdr = (uint32_t)strtoul(tmp, NULL, 16);
		return hdr == frame_id;
	}

	// Case B: split header bytes: "00 00 08 C0" (extended) or "00 FF" (standard)
	if (t1_len == 2 && elm327_is_hex_n(t1, 2))
	{
		// token 2
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		const char *t2 = p;
		while (p < end && *p != ' ' && *p != '\t')
			p++;
		size_t t2_len = (size_t)(p - t2);

		// token 3
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		const char *t3 = p;
		while (p < end && *p != ' ' && *p != '\t')
			p++;
		size_t t3_len = (size_t)(p - t3);

		// token 4
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		const char *t4 = p;
		while (p < end && *p != ' ' && *p != '\t')
			p++;
		size_t t4_len = (size_t)(p - t4);

		// Try extended 4-byte header first
		if (t2_len == 2 && t3_len == 2 && t4_len == 2 &&
			elm327_is_hex_n(t2, 2) && elm327_is_hex_n(t3, 2) && elm327_is_hex_n(t4, 2))
		{
			uint8_t b1 = (uint8_t)strtoul((char[3]){t1[0], t1[1], 0}, NULL, 16);
			uint8_t b2 = (uint8_t)strtoul((char[3]){t2[0], t2[1], 0}, NULL, 16);
			uint8_t b3 = (uint8_t)strtoul((char[3]){t3[0], t3[1], 0}, NULL, 16);
			uint8_t b4 = (uint8_t)strtoul((char[3]){t4[0], t4[1], 0}, NULL, 16);
			uint32_t hdr32 = ((uint32_t)b1 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b3 << 8) | (uint32_t)b4;
			if (hdr32 == frame_id)
				return true;
		}

		// Try 2-byte header for standard
		if (t2_len == 2 && elm327_is_hex_n(t2, 2))
		{
			uint8_t b1 = (uint8_t)strtoul((char[3]){t1[0], t1[1], 0}, NULL, 16);
			uint8_t b2 = (uint8_t)strtoul((char[3]){t2[0], t2[1], 0}, NULL, 16);
			uint32_t hdr16 = ((uint32_t)b1 << 8) | (uint32_t)b2;
			return hdr16 == frame_id;
		}
	}

	return false;
}

void elm327_run_command(char* command, uint32_t command_len, uint32_t timeout, QueueHandle_t *response_q, response_callback_t response_callback, bool stop_after_first_frame, uint32_t expected_frame_id)
{
	if (xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
    {
		uint32_t len;
		bool terminator_received = false;
		uart_event_t event;
		static uint8_t *uart_read_buf = NULL;
		static size_t uart_read_buf_size = 0;
		bool last_was_cr = false;
		bool use_timeout = (timeout > 0);
		wc_timer_t timeout_timer = 0;
		bool is_atma = false;
		bool atma_stop_sent = false;
		bool atma_frame_seen = false;
		char line_buf[96];
		size_t line_len = 0;
		
		if(uart_read_buf == NULL)
		{
			uart_read_buf_size = (size_t)ELM327_MAX_CMD_LEN;
			uart_read_buf = (uint8_t *)heap_caps_malloc(uart_read_buf_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
			if (!uart_read_buf)
			{
				ESP_LOGE(TAG, "Failed to allocate uart_read_buf");
				xSemaphoreGive(xuart1_semaphore);
				return;
			}
			memset(uart_read_buf, 0, uart_read_buf_size + 1);
		}

		if(command_len == 0)
		{
			len = strlen(command);
		}
		else
		{
			len = command_len;
		}

		// Detect ATMA (monitor-all) command: it does not normally return a prompt until a key is sent.
		// IMPORTANT: to stop ATMA, send a non-CR key (e.g. space). Sending '\r' can trigger
		// "repeat last command" behavior on some ELM/STN chips and may re-enter ATMA.
		{
			// Simple case-insensitive scan ignoring spaces
			char c0 = 0, c1 = 0, c2 = 0, c3 = 0;
			uint32_t i = 0;
			// find 'A'
			while (i < len && (command[i] == ' ' || command[i] == '\t')) i++;
			if (i < len) c0 = command[i++];
			while (i < len && (command[i] == ' ' || command[i] == '\t')) i++;
			if (i < len) c1 = command[i++];
			while (i < len && (command[i] == ' ' || command[i] == '\t')) i++;
			if (i < len) c2 = command[i++];
			while (i < len && (command[i] == ' ' || command[i] == '\t')) i++;
			if (i < len) c3 = command[i++];
			// Normalize to lower-case
			if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
			if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
			if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
			if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 - 'A' + 'a');
			is_atma = (c0 == 'a' && c1 == 't' && c2 == 'm' && c3 == 'a');
		}
		uart_flush_input(UART_NUM_1);
		xQueueReset(uart1_queue);


		if(strstr(command, "ATZ\r") != NULL || 
		strstr(command, "atz\r") != NULL ||
		strstr(command, "AT Z\r") != NULL ||
		strstr(command, "at z\r") != NULL)
		{
			// if(strlen(elm327_command.command) < sizeof(last_command))
			// {
			// 	strcpy(last_command, elm327_command.command);
			// }
			elm327_uart_write_bytes(UART_NUM_1, "ATWS\r", strlen("ATWS\r"));
			uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
		}
		else
		{
			elm327_uart_write_bytes(UART_NUM_1, command, len);
		}

		// Start timeout window after command TX.
		if (use_timeout)
		{
			wc_timer_set(&timeout_timer, timeout);
		}
		
		while (!terminator_received)
		{
			// If ATMA is configured to stop after first frame, and we've seen a frame, stop ASAP.
			if (is_atma && stop_after_first_frame && atma_frame_seen && !atma_stop_sent)
			{
				// Use space (0x20) to stop ATMA without repeating the last command.
				elm327_uart_write_bytes(UART_NUM_1, " ", 1);
				(void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
				atma_stop_sent = true;
				// After requesting stop, keep draining until prompt for a short grace window.
				wc_timer_t grace_timer = 0;
				wc_timer_set(&grace_timer, 200);
				while (!terminator_received && !wc_timer_is_expired(&grace_timer))
				{
					if (xQueueReceive(uart1_queue, (void*)&event, pdMS_TO_TICKS(20)) == pdTRUE)
					{
						if (event.type == UART_DATA)
						{
							uint32_t remaining = event.size;
							while (remaining > 0 && !terminator_received)
							{
								uint32_t to_read = remaining;
								if (to_read > uart_read_buf_size)
									to_read = (uint32_t)uart_read_buf_size;
								int read_bytes = elm327_uart_read_bytes(UART_NUM_1, uart_read_buf, to_read, pdMS_TO_TICKS(1));
								if (read_bytes <= 0)
									break;
								uart_read_buf[read_bytes] = '\0';
								if (response_callback != NULL)
								{
									response_callback((char*)uart_read_buf, read_bytes, response_q, command);
								}
								if (elm327_scan_for_prompt(uart_read_buf, read_bytes, &last_was_cr))
								{
									terminator_received = true;
									break;
								}
								remaining -= (uint32_t)read_bytes;
							}
						}
					}
				}
				break;
			}

			if (use_timeout)
			{
				if (wc_timer_is_expired(&timeout_timer))
				{
					// Timeout reached. For ATMA, send a key (space) to stop monitor mode.
					if (is_atma)
					{
						// Use space (0x20) to stop ATMA without repeating the last command.
						elm327_uart_write_bytes(UART_NUM_1, " ", 1);
						(void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));
						atma_stop_sent = true;
						// Give a short grace period to receive the prompt
						wc_timer_t grace_timer = 0;
						wc_timer_set(&grace_timer, 200);
						while (!terminator_received && !wc_timer_is_expired(&grace_timer))
						{
							if (xQueueReceive(uart1_queue, (void*)&event, pdMS_TO_TICKS(20)) == pdTRUE)
							{
								if (event.type == UART_DATA)
								{
									uint32_t remaining = event.size;
									while (remaining > 0 && !terminator_received)
									{
										uint32_t to_read = remaining;
										if (to_read > uart_read_buf_size)
											to_read = (uint32_t)uart_read_buf_size;
										int read_bytes = elm327_uart_read_bytes(UART_NUM_1, uart_read_buf, to_read, pdMS_TO_TICKS(1));
										if (read_bytes <= 0)
											break;
										uart_read_buf[read_bytes] = '\0';
										if (response_callback != NULL)
										{
											response_callback((char*)uart_read_buf, read_bytes, response_q, command);
										}
										if (elm327_scan_for_prompt(uart_read_buf, read_bytes, &last_was_cr))
										{
											terminator_received = true;
											break;
										}
										remaining -= (uint32_t)read_bytes;
									}
								}
							}
						}
					}
					break;
				}
			}

			int queue_wait_ms = use_timeout ? 20 : ELM327_CMD_TIMEOUT_MS;
			if (xQueueReceive(uart1_queue, (void*)&event, pdMS_TO_TICKS(queue_wait_ms)) == pdTRUE)
			{
				if (event.type == UART_DATA)
				{
					uint32_t remaining = event.size;
					while (remaining > 0 && !terminator_received)
					{
						uint32_t to_read = remaining;
						if (to_read > uart_read_buf_size)
							to_read = (uint32_t)uart_read_buf_size;
						int read_bytes = elm327_uart_read_bytes(UART_NUM_1, uart_read_buf, to_read, pdMS_TO_TICKS(1));
						if (read_bytes <= 0)
							break;
						uart_read_buf[read_bytes] = '\0';

						// For ATMA stop-after-first-frame mode, detect the first matching frame line.
						if (is_atma && stop_after_first_frame && !atma_frame_seen)
						{
							for (int i = 0; i < read_bytes; i++)
							{
								char b = (char)uart_read_buf[i];
								if (b == '\r' || b == '\n')
								{
									if (line_len > 0)
									{
										if (is_likely_can_frame_line(line_buf, line_len))
										{
											if (expected_frame_id == 0 || elm327_atma_line_matches_frame_id(line_buf, line_len, expected_frame_id))
											{
												atma_frame_seen = true;
											}
										}
									}
									line_len = 0;
								}
								else
								{
									if (line_len + 1 < sizeof(line_buf))
									{
										line_buf[line_len++] = b;
									}
								}
							}
						}

						if(response_callback != NULL)
						{
							response_callback((char*)uart_read_buf, read_bytes, response_q, command);
						}

						if (elm327_scan_for_prompt(uart_read_buf, read_bytes, &last_was_cr))
						{
							terminator_received = true;
							break;
						}

						remaining -= (uint32_t)read_bytes;
					}
				}
			}
		}
		xSemaphoreGive(xuart1_semaphore);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take semaphore", __func__);
	}
}

elm327_chip_status_t elm327_chip_get_status(void)
{
	elm327_chip_status_t status = gpio_get_level(OBD_READY_PIN);

	return status;
}

static int get_one_record(const uint8_t* data, int size, int index, char* record)
{
    bool found_line_end = false;
    int count = 0;
    
    while (index < size)
	{
        uint8_t byte = data[index++];
        
        if (byte == '\r' || byte == '\n')
		{
            if (!found_line_end) 
			{
                found_line_end = true;
                continue;
            }
            if (found_line_end && count > 0) break;
            continue;
        }
        
        if (byte >= 'a' && byte <= 'z')
		{
            byte -= ('a' - 'A');
        }
        
        if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F'))
		{
            record[count++] = byte;
        }
    }
    
    record[count] = '\0';
    return index;
}

esp_err_t elm327_send_update_command(const char* cmd, char** response, size_t* response_size, int timeout_ms) 
{
	int len = 0;
	uint8_t byte_data = 0;
	const size_t cmd_len = strlen(cmd);
	bool echo_seen = false;
	bool overflow = false;
	esp_err_t ret = ESP_FAIL;

	if(update_resp_buf == NULL)
	{
		return ESP_ERR_NO_MEM;
	}
	// Clear buffer and set out pointers
	memset(update_resp_buf, 0, ELM327_UPDATE_BUF_SIZE);
	*response = update_resp_buf;
	*response_size = 0;

	// Flush any stale RX data before sending a new command
	uart_flush_input(UART_NUM_1);

	ESP_LOGD(TAG, "Sending: %s", cmd);
	int written = elm327_uart_write_bytes(UART_NUM_1, cmd, cmd_len);
	if (written < 0)
	{
		ESP_LOGE(TAG, "Failed to write command");
		return ESP_FAIL;
	}
	// Ensure bytes are pushed out before we start reading a response
	(void)uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(50));

	const int64_t start_time = esp_timer_get_time() / 1000;
	while (true)
	{
		if ((esp_timer_get_time() / 1000 - start_time) >= timeout_ms)
		{
			ESP_LOGE(TAG, "Command timeout");
			ret = ESP_ERR_TIMEOUT;
			break;
		}

		if (elm327_uart_read_bytes(UART_NUM_1, &byte_data, 1, pdMS_TO_TICKS(100)) != 1)
		{
			continue;
		}

		// Store into buffer if space available
		if (len < ELM327_UPDATE_BUF_SIZE - 1)
		{
			update_resp_buf[len] = (char)byte_data;
		}
		else
		{
			// Overflow protection: mark and keep reading until terminator
			overflow = true;
		}

		// Detect and skip echo: if the accumulated bytes equal the command, drop them
		if (!echo_seen)
		{
			int cmp_len = len + 1; // include current byte
			if (cmp_len <= (int)cmd_len && strncmp(update_resp_buf, cmd, cmp_len) == 0)
			{
				if (cmp_len == (int)cmd_len)
				{
					// Full echo seen, reset buffer
					len = 0;
					memset(update_resp_buf, 0, ELM327_UPDATE_BUF_SIZE);
					echo_seen = true;
					continue;
				}
				// Keep collecting until full echo matched
				len++;
				continue;
			}
			else if (cmp_len <= (int)cmd_len)
			{
				// Not matching echo, keep as part of response
			}
		}

		// Early-exit for VTDLED: OK followed by CR
		if (strstr(cmd, "VTDLED") && len >= 1)
		{
			if (byte_data == '\r' && strstr(update_resp_buf, "OK"))
			{
				len++;
				update_resp_buf[len] = '\0';
				ret = ESP_OK;
				break;
			}
		}

		if (byte_data == '>')
		{
			len++;
			update_resp_buf[len] = '\0';
			break;
		}

		if (byte_data != 0)
		{
			len++;
		}
	}

	*response_size = (size_t)len;

	if (overflow)
	{
		ESP_LOGE(TAG, "Response buffer overflow (size=%d). Truncated.", len);
		ret = ESP_ERR_INVALID_SIZE;
	}

	if (ret != ESP_ERR_TIMEOUT && ret != ESP_ERR_INVALID_SIZE)
	{
		// Classify response content
		if (strstr(update_resp_buf, "OK") || strstr(update_resp_buf, "MIC3624"))
		{
			ret = ESP_OK;
		}
		else if (strstr(update_resp_buf, "?"))
		{
			ret = ESP_ERR_NOT_FOUND;
		}
		else
		{
			ESP_LOGW(TAG, "Unexpected response: %s", update_resp_buf);
			ret = ESP_FAIL;
		}
	}

	if (strstr(cmd, "VTDLED"))
	{
		ESP_LOGW(TAG, "VTDLED command sent");
		ESP_LOG_BUFFER_HEXDUMP(TAG, (const void*)update_resp_buf, len, ESP_LOG_INFO);
	}

	return ret;
}

esp_err_t elm327_check_obd_device() 
{
	char* response = NULL;
	size_t response_size = 0;
    device_status.in_normal_state = false;
	device_status.need_update = false;

	if(update_resp_buf == NULL)
	{
		return ESP_ERR_NO_MEM;
	}

    esp_err_t ret = elm327_send_update_command("VTVERS\r", &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
	
	if (response)
	{
		ESP_LOG_BUFFER_HEXDUMP(TAG, response, strlen(response), ESP_LOG_INFO);
		if (ret != ESP_OK)
		{
			if (ret == ESP_ERR_NOT_FOUND)
			{
				device_status.device_type = 0xFF;
				ret = ESP_OK;
			}
		}
		else if (strncmp(response, "MIC3624", 7) == 0)
		{
			device_status.in_normal_state = true;
			device_status.device_type = 0x3624;
			if (strstr(response, OBD_FW_VER_V22) != NULL)
			{
				ESP_LOGI(TAG, "ELM327 OBD Firmware is already up to date.");
			}
			else
			{
				device_status.need_update = true;
			}

			ret = ESP_OK;
		}
		else
		{
			ESP_LOGE(TAG, "Unknown device");
			ret = ESP_FAIL;
		}
	}
    
    return ret;
}

static void elm327_disable_wake_commands(void)
{
	ESP_LOGI(TAG, "Disabling wake commands");
	if(xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
	{
		char* rx_buffer = (char*)heap_caps_malloc(ELM327_CMD_BUFFER_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		if(rx_buffer == NULL)
		{
			ESP_LOGE(TAG, "Failed to allocate memory for rx_buffer");
			xSemaphoreGive(xuart1_semaphore);
			return;
		}
		bzero(rx_buffer, ELM327_CMD_BUFFER_SIZE);
		uart_flush_input(UART_NUM_1);
		//make sure chip goes to sleep
		elm327_uart_write_bytes(UART_NUM_1, "ATPP 0F SV 95\r", strlen("ATPP 0F SV 95\r"));
		uart_read_until_pattern(UART_NUM_1, rx_buffer, ELM327_CMD_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
		bzero(rx_buffer, ELM327_CMD_BUFFER_SIZE);
		vTaskDelay(pdMS_TO_TICKS(100));
		elm327_uart_write_bytes(UART_NUM_1, "ATPP 0F ON\r", strlen("ATPP 0F ON\r"));
		uart_read_until_pattern(UART_NUM_1, rx_buffer, ELM327_CMD_BUFFER_SIZE - 1, "\r>", UART_TIMEOUT_MS);
		ESP_LOGI(TAG, "Wake commands disabled");
		ESP_LOG_BUFFER_HEXDUMP(TAG, (const void*)rx_buffer, strlen(rx_buffer), ESP_LOG_INFO);
		vTaskDelay(pdMS_TO_TICKS(100));
		xSemaphoreGive(xuart1_semaphore);
		free(rx_buffer);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take UART semaphore", __func__);
	}

	elm327_hardreset_chip();
}

esp_err_t elm327_update_obd(bool force_update)
{
    const char* current_ptr = (const char*)obd_fw_start;
    const char* end_ptr = (const char*)obd_fw_end;
	esp_err_t ret = ESP_FAIL;

	if(xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
    {
		ret = elm327_check_obd_device();

		if(device_status.need_update == false && force_update == false && device_status.in_normal_state == true)
		{
			xSemaphoreGive(xuart1_semaphore);
			return ret;
		}

		if(device_status.in_normal_state == false)
		{
			ESP_LOGE(TAG, "Device is not in normal state, attempting to update");
		}

		ESP_LOGW(TAG, "MIC3624 Start update");
		
		char* response = NULL;
		size_t response_size = 0;
		
		// Enter update mode
		ret = elm327_send_update_command("VTDLMIC3422\r", &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
		if (ret != ESP_OK)
		{
			xSemaphoreGive(xuart1_semaphore);
			return ret;
		}
		// Response buffer is static; do not free
		response = NULL;
		led_fast_blink(LED_RED, 255, true);

		uint32_t line_count = 0;
		bool update_complete = false;
		static char line[ELM327_UPDATE_MAX_LINE_LENGTH];
		
		// Process embedded firmware data
		while (current_ptr < end_ptr)
		{
			// Copy line until newline or end of data
			size_t i = 0;
			while (current_ptr < end_ptr && i < (ELM327_UPDATE_MAX_LINE_LENGTH - 1) && *current_ptr != '\n')
			{
				line[i++] = *current_ptr++;
			}
			line[i] = '\0';
			
			// Skip remaining newline character if present
			if (current_ptr < end_ptr && *current_ptr == '\n')
			{
				current_ptr++;
			}

			line_count++;
			size_t len = strlen(line);

			// Remove carriage return if present
			if (len > 0 && line[len-1] == '\r')
			{
				line[--len] = '\0';
			}

			// Skip empty lines
			if (len == 0)
			{
				continue;
			}

			// Check for end marker
			if (strncmp(line, "FFF1", 4) == 0)
			{
				update_complete = true;
				break;
			}

			// Prepare and send command using stack buffer, no dynamic allocation
			{
				char cmd[ELM327_UPDATE_MAX_LINE_LENGTH + 8]; // "VTDLDT" + line + \r + \0
				memset(cmd, 0, sizeof(cmd));
				snprintf(cmd, sizeof(cmd), "VTDLDT%.*s\r", (int)len, line);
				ret = elm327_send_update_command(cmd, &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
			}
			if (ret != ESP_OK)
			{
				ESP_LOGE(TAG, "Failed at line %lu", line_count);
				break;
			}

			vTaskDelay(pdMS_TO_TICKS(1));
		}

		// End update if everything was successful
		if (ret == ESP_OK && update_complete)
		{
			vTaskDelay(pdMS_TO_TICKS(2000));
			ESP_LOGI(TAG, "Finalizing update with command VTDLED...");
			// Retry up to 3 times to finalize
			ret = ESP_FAIL; // ensure we enter the loop
			for (uint8_t attempt = 1; attempt <= 3; ++attempt)
			{
				response = NULL;
				esp_err_t r = elm327_send_update_command("VTDLED\r", &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
				if (r == ESP_OK)
				{
					ret = r;
					ESP_LOGI(TAG, "VTDLED succeeded on attempt %u", attempt);
					break;
				}
				ret = r;
				ESP_LOGW(TAG, "VTDLED attempt %u failed: %s", attempt, esp_err_to_name(r));
				vTaskDelay(pdMS_TO_TICKS(200));
			}
			if (ret != ESP_OK)
			{
				ESP_LOGE(TAG, "Failed to finalize update after retries");
			}
			vTaskDelay(pdMS_TO_TICKS(100));
		}
		
		ESP_LOGW(TAG, "ELM327 chip update DONE!");
		uart_set_baudrate(UART_NUM_1, DESIRED_BAUD_RATE);
		vTaskDelay(pdMS_TO_TICKS(2000));

		gpio_set_level(OBD_RESET_PIN, 0);
		vTaskDelay(pdMS_TO_TICKS(5));
		gpio_set_level(OBD_RESET_PIN, 1);

		uart_flush_input(UART_NUM_1);
		vTaskDelay(pdMS_TO_TICKS(2000));

		xSemaphoreGive(xuart1_semaphore);
	}
	else
	{
		ESP_LOGE(TAG, "%s: Failed to take semaphore", __func__);
	}

    elm327_hardreset_chip();
	vTaskDelay(pdMS_TO_TICKS(2000));
	elm327_disable_wake_commands();
	
	led_fast_blink(LED_RED, 0, false);
	ESP_LOGI(TAG, "ELM327 chip update DONE! Rebooting...");

	return ret;
}

esp_err_t elm327_update_obd_from_file(const char* filename)
{
    FILE* file = fopen(filename, "r");
    if (!file)
	{
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return ESP_FAIL;
    }

	// Stack line buffer (no dynamic memory)
	char line[ELM327_UPDATE_MAX_LINE_LENGTH];

	esp_err_t ret = elm327_check_obd_device();
	if (ret != ESP_OK)
	{
		fclose(file);
		return ret;
	}
	ESP_LOGW(TAG, "MIC3624 Start update");
    char* response;
    size_t response_size;
    
    // Enter update mode
	ret = elm327_send_update_command("VTDLMIC3422\r", &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
	if (ret != ESP_OK)
	{
		fclose(file);
		return ret;
	}

	led_fast_blink(LED_RED, 255, true);

    uint32_t line_count = 0;
    bool update_complete = false;

    // Process file
	while (fgets(line, sizeof(line), file))
	{
        size_t len = strlen(line);
        line_count++;

        // Remove line endings
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
		{
            line[--len] = '\0';
        }

        // Skip empty lines
        if (len == 0)
		{
            continue;
        }

        // Check for end marker
        if (strncmp(line, "FFF1", 4) == 0)
		{
            update_complete = true;
            break;
        }

		// Prepare and send command (stack buffer, no dynamic memory)
		{
			char cmd[ELM327_UPDATE_MAX_LINE_LENGTH + 8];
			snprintf(cmd, sizeof(cmd), "VTDLDT%.*s\r", (int)len, line);
			ret = elm327_send_update_command(cmd, &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
		}
        
        if (ret != ESP_OK)
		{
            ESP_LOGE(TAG, "Failed at line %lu", line_count);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Cleanup
    fclose(file);

    // End update if everything was successful
    if (ret == ESP_OK && update_complete)
	{
		ESP_LOGI(TAG, "Finalizing update with command VTDLED...");
		// Retry finalize up to 3 times
		for (uint8_t attempt = 1; attempt <= 3 && ret != ESP_OK; ++attempt)
		{
			esp_err_t r = elm327_send_update_command("VTDLED\r", &response, &response_size, ELM327_UPDATE_TIMEOUT_MS);
			ret = r;
			if (r != ESP_OK)
			{
				ESP_LOGW(TAG, "VTDLED attempt %u failed: %s", attempt, esp_err_to_name(r));
				vTaskDelay(pdMS_TO_TICKS(200));
			}
		}
		if (ret == ESP_OK)
		{
			ESP_LOGI(TAG, "Update finalized successfully");
		}
		else
		{
			ESP_LOGE(TAG, "Failed to finalize update");
		}
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

	ESP_LOGW(TAG, "ELM327 chip update DONE!");
	elm327_hardreset_chip();
	elm327_disable_wake_commands();
	led_fast_blink(LED_RED, 0, false);
	
    return ret;
}

void elm327_read_task(void *pvParameters)
{
    uart_event_t event;
	static xdev_buffer dtmp;
    // uint8_t* dtmp = (uint8_t*) malloc(UART_BUF_SIZE);
    
    while(1) 
    {
		dev_status_wait_for_bits(DEV_AWAKE_BIT, portMAX_DELAY);
        if(xQueuePeek(uart1_queue, (void *)&event, portMAX_DELAY)) 
        {
			if(xSemaphoreTake(xuart1_semaphore, pdMS_TO_TICKS(ELM327_CMD_MUTEX_TIMOUT)) == pdTRUE)
			{
				bzero(dtmp.ucElement, sizeof(dtmp.ucElement));
				// TODO: fix this. Here it's checking if queu is not empty, other task might have processed the queue while waiting for empty queue
				if((xQueuePeek(uart1_queue, (void *)&event, 0)) == pdTRUE && xQueueReceive(uart1_queue, (void *)&event, portMAX_DELAY) == pdTRUE)
				{
					switch(event.type) 
					{
						case UART_DATA:
							elm327_uart_read_bytes(UART_NUM_1, dtmp.ucElement, event.size, portMAX_DELAY);
							
							if(elm327_response != NULL)
							{
								ESP_LOG_BUFFER_HEXDUMP(TAG, (char*)dtmp.ucElement, event.size, ESP_LOG_INFO);
								dtmp.usLen = event.size;
								// ESP_LOG_BUFFER_CHAR(TAG, (char*)dtmp, event.size);
								elm327_response((char*)dtmp.ucElement, 
														event.size, 
														xqueue_elm327_uart_rx, 
														NULL);
							}
							break;
						case UART_FIFO_OVF:
							uart_flush_input(UART_NUM_1);
							xQueueReset(uart1_queue);
							break;
						case UART_BUFFER_FULL:
							uart_flush_input(UART_NUM_1);
							xQueueReset(uart1_queue);
							break;
						default:
							break;
					}
				}
				xSemaphoreGive(xuart1_semaphore);
			}
			else
			{
				ESP_LOGE(TAG, "%s: Failed to take semaphore", __func__);
				vTaskDelay(pdMS_TO_TICKS(100));
			}
        }
    }
    
    vTaskDelete(NULL);
}


void elm327_init(response_callback_t rsp_callback, QueueHandle_t *rx_queue, void (*can_log)(twai_message_t* frame, uint8_t type), bool udp_log_enabled)
{
    xqueue_elm327_uart_rx = rx_queue;
    elm327_can_log = can_log;
	elm327_response = rsp_callback;
	elm327_udp_log_enabled = udp_log_enabled;
	if(elm327_udp_log_enabled)
	{
		ESP_LOGW(TAG, "ELM327 UDP logging enabled");
	}

	if(update_resp_buf == NULL)
	{
		update_resp_buf = (char *)heap_caps_malloc(ELM327_UPDATE_BUF_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		memset(update_resp_buf, 0, ELM327_UPDATE_BUF_SIZE);
	}


	if(s_elm_tx_buf == NULL)
	{
		s_elm_tx_buf = (char *)heap_caps_malloc(ELM327_UART_LOG_BUF_SZ, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		memset(s_elm_tx_buf, 0, ELM327_UART_LOG_BUF_SZ);
	}
	if(s_elm_rx_buf == NULL)
	{
		s_elm_rx_buf = (char *)heap_caps_malloc(ELM327_UART_LOG_BUF_SZ, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		memset(s_elm_rx_buf, 0, ELM327_UART_LOG_BUF_SZ);
	}
	if(emit_line_buf == NULL)
	{
		emit_line_buf = (char *)heap_caps_malloc(ELM327_UART_LOG_BUF_SZ, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
		memset(emit_line_buf, 0, ELM327_UART_LOG_BUF_SZ);
	}

	if (elm327_cmd_queue_storage == NULL)
	{
		size_t storage_size = ELM327_CMD_QUEUE_SIZE * sizeof(elm327_commands_t);
		#if defined(CONFIG_SPIRAM)
		elm327_cmd_queue_storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		#endif
		if (elm327_cmd_queue_storage == NULL)
		{
			ESP_LOGW(TAG, "Queue storage PSRAM alloc failed; falling back to internal RAM");
			elm327_cmd_queue_storage = (uint8_t *)heap_caps_malloc(storage_size, MALLOC_CAP_8BIT);
		}
		if (elm327_cmd_queue_storage)
		{
			memset(elm327_cmd_queue_storage, 0, storage_size);
		}
	}

	if (elm327_cmd_queue_storage == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate queue storage");
	}

    elm327_cmd_queue = xQueueCreateStatic(ELM327_CMD_QUEUE_SIZE, sizeof(elm327_commands_t), elm327_cmd_queue_storage, &elm327_cmd_queue_struct);
	if (elm327_cmd_queue == NULL){
		ESP_LOGE(TAG, "Failed to create queue");
	}

	xuart1_semaphore = xSemaphoreCreateMutex();
	if (xuart1_semaphore == NULL) {
		ESP_LOGE(TAG, "Failed to create semaphore");
	}
	
    uart_config_t uart1_config = 
    {
        .baud_rate = DESIRED_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_NUM_1, UART_BUF_SIZE, UART_BUF_SIZE, 
                                      100, &uart1_queue, 0);
    if (ret != ESP_OK) 
    {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
    }

    uart_param_config(UART_NUM_1, &uart1_config);
    uart_set_pin(UART_NUM_1, GPIO_NUM_16, GPIO_NUM_15, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

	vTaskDelay(pdMS_TO_TICKS(50));
	elm327_hardreset_chip();

	// elm327_update_obd_from_file("/sdcard/MIC3624_v2.3.07.beta4.txt");
	// elm327_update_obd_from_file("/sdcard/MIC3624_v2.3.10.txt");
	// elm327_update_obd_from_file("/sdcard/MIC3624_v2.3.18.txt");
	elm327_update_obd(false);
	elm327_powerpin_commands();
	static uint8_t status_not_ready_count = 0;
	while(elm327_chip_get_status() != ELM327_READY)
	{
		ESP_LOGW(TAG, "ELM327 not ready...");

		if(status_not_ready_count++ > 10)
		{
			ESP_LOGE(TAG, "ELM327 not ready for too long, hardreset chip");
			elm327_hardreset_chip();
			status_not_ready_count = 0;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	status_not_ready_count = 0;
	
    uart_flush(UART_NUM_1);

    obd_init();

    static StackType_t *uart1_event_task_stack, *elm327_read_task_stack;
    static StaticTask_t uart1_event_task_buffer, elm327_read_task_buffer;
    
	// Keep stacks in internal RAM (external/PSRAM stacks can crash if caches are temporarily disabled).
	uart1_event_task_stack = heap_caps_malloc((2048*2) * sizeof(StackType_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
	elm327_read_task_stack = heap_caps_malloc((2048*2) * sizeof(StackType_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    
    if (uart1_event_task_stack == NULL || elm327_read_task_stack == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate task stack memory");
        if (uart1_event_task_stack) heap_caps_free(uart1_event_task_stack);
        if (elm327_read_task_stack) heap_caps_free(elm327_read_task_stack);
        return;
    }
    
    // Create static tasks
    TaskHandle_t uart1_event_task_handle = xTaskCreateStatic(
        uart1_event_task,
        "uart1_event_task",
        2048*2,
        NULL,
        12,
        uart1_event_task_stack,
        &uart1_event_task_buffer
    );
    
    TaskHandle_t elm327_read_task_handle = xTaskCreateStatic(
        elm327_read_task,
        "elm327_read_task",
        2048*2,
        NULL,
        12,
        elm327_read_task_stack,
        &elm327_read_task_buffer
    );
    
    if (uart1_event_task_handle == NULL || elm327_read_task_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create tasks");
        if (uart1_event_task_handle == NULL) heap_caps_free(uart1_event_task_stack);
        if (elm327_read_task_handle == NULL) heap_caps_free(elm327_read_task_stack);
    }

}

#endif
