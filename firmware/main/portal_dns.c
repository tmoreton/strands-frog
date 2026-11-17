#include "portal_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_HEADER_SIZE 12
#define DNS_MAX_PACKET 1232
#define DNS_PORT 53
#define DNS_OPT 41

typedef struct {
    int socket_fd;
    uint32_t ipv4_be;
} dns_context_t;

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] << 8 | p[1];
}

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value;
}

static bool skip_name(const uint8_t *packet, size_t length, size_t *offset, bool compressed)
{
    size_t cursor = *offset;
    size_t wire_end = 0;
    size_t expanded = 0;
    unsigned jumps = 0;
    while (cursor < length) {
        size_t label_offset = cursor;
        uint8_t label = packet[cursor++];
        if ((label & 0xc0) == 0xc0 && compressed) {
            if (cursor >= length || ++jumps > 32) {
                return false;
            }
            size_t target = ((size_t)(label & 0x3f) << 8) | packet[cursor++];
            if (target < DNS_HEADER_SIZE || target >= label_offset) {
                return false;
            }
            if (wire_end == 0) {
                wire_end = cursor;
            }
            cursor = target;
        } else {
            expanded += label + 1;
            if (label > 63 || label > length - cursor || expanded > 255) {
                return false;
            }
            if (label == 0) {
                *offset = wire_end != 0 ? wire_end : cursor;
                return true;
            }
            cursor += label;
        }
    }
    return false;
}

static bool valid_options(const uint8_t *packet, size_t offset, size_t end)
{
    while (offset < end) {
        if (end - offset < 4) {
            return false;
        }
        size_t length = read_u16(packet + offset + 2);
        offset += 4;
        if (length > end - offset) {
            return false;
        }
        offset += length;
    }
    return true;
}

static bool valid_additional(const uint8_t *packet, size_t length, size_t offset, uint16_t count)
{
    bool seen_opt = false;
    for (uint16_t i = 0; i < count; ++i) {
        size_t name_start = offset;
        if (!skip_name(packet, length, &offset, true) || length - offset < 10) {
            return false;
        }
        uint16_t type = read_u16(packet + offset);
        size_t data_length = read_u16(packet + offset + 8);
        offset += 10;
        if (data_length > length - offset) {
            return false;
        }
        if (type == DNS_OPT) {
            if (seen_opt || packet[name_start] != 0 ||
                !valid_options(packet, offset, offset + data_length)) {
                return false;
            }
            seen_opt = true;
        }
        offset += data_length;
    }
    return offset == length;
}

static size_t dns_reply(uint8_t *packet, size_t length, uint32_t ipv4_be)
{
    if (length < DNS_HEADER_SIZE || length > DNS_MAX_PACKET) {
        return 0;
    }
    uint16_t flags = read_u16(packet + 2);
    const uint16_t query_flags = 0x0130;  // RD, AD, CD
    if ((flags & ~query_flags) || read_u16(packet + 4) != 1 ||
        read_u16(packet + 6) != 0 || read_u16(packet + 8) != 0) {
        return 0;
    }

    // The first and only question has no earlier domain name to refer to.
    size_t question_end = DNS_HEADER_SIZE;
    if (!skip_name(packet, length, &question_end, false) || length - question_end < 4) {
        return 0;
    }
    bool answer_a = read_u16(packet + question_end) == 1 &&
                    read_u16(packet + question_end + 2) == 1;
    question_end += 4;
    if (!valid_additional(packet, length, question_end, read_u16(packet + 10))) {
        return 0;
    }

    write_u16(packet + 2, 0x8400 | (flags & 0x0110));  // QR, AA, echo RD and CD
    write_u16(packet + 6, answer_a ? 1 : 0);
    write_u16(packet + 10, 0);  // Additional data, including EDNS, is not echoed.
    if (!answer_a) {
        return question_end;
    }

    // Compressed owner name points at the question. Type A, class IN, TTL 0.
    static const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
    memcpy(packet + question_end, record, sizeof(record));
    memcpy(packet + question_end + sizeof(record), &ipv4_be, sizeof(ipv4_be));
    return question_end + sizeof(record) + sizeof(ipv4_be);
}

static void dns_task(void *argument)
{
    const dns_context_t context = *(dns_context_t *)argument;
    free(argument);
    // The extra byte lets us reject datagrams truncated to our receive buffer.
    uint8_t packet[DNS_MAX_PACKET + 1];
    for (;;) {
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        int received = recvfrom(context.socket_fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&client, &client_length);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        size_t reply_length = dns_reply(packet, received, context.ipv4_be);
        if (reply_length != 0) {
            sendto(context.socket_fd, packet, reply_length, 0,
                   (struct sockaddr *)&client, client_length);
        }
    }
    close(context.socket_fd);
    vTaskDelete(NULL);
}

esp_err_t portal_dns_start(uint32_t ipv4_be)
{
    if (ipv4_be == 0 || (ntohl(ipv4_be) >> 24) >= 224) {
        return ESP_ERR_INVALID_ARG;
    }
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        return ESP_FAIL;
    }
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = ipv4_be,
    };
    if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return ESP_FAIL;
    }

    dns_context_t *context = malloc(sizeof(*context));
    if (context == NULL) {
        close(socket_fd);
        return ESP_ERR_NO_MEM;
    }
    *context = (dns_context_t){.socket_fd = socket_fd, .ipv4_be = ipv4_be};
    if (xTaskCreate(dns_task, "portal_dns", 4096, context, 4, NULL) != pdPASS) {
        free(context);
        close(socket_fd);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
