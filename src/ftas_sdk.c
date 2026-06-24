#include "ftas_sdk.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>

#define FTAS_HDR_SIZE 8
#define FTAS_MAX_MSG_SIZE 2048
#define FTAS_TX_PARAM_FIXED_PAYLOAD 50
#define FTAS_IQ_FIXED_BODY_BYTES 30

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_le64(uint8_t *p, uint64_t v) {
    wr_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr_le32(p + 4, (uint32_t)(v >> 32));
}

static int set_socket_timeout_ms(int fd, int timeout_ms) {
    struct timeval tv;

    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return FTAS_ERR_SOCKET;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return FTAS_ERR_SOCKET;
    }
    return FTAS_OK;
}

static int write_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n == 0) {
            return FTAS_ERR_SEND;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return FTAS_ERR_TIMEOUT;
            }
            return FTAS_ERR_SEND;
        }
        sent += (size_t)n;
    }
    return FTAS_OK;
}

static int read_all(int fd, uint8_t *buf, size_t len) {
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, buf + recvd, len - recvd, 0);
        if (n == 0) {
            return FTAS_ERR_RECV;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return FTAS_ERR_TIMEOUT;
            }
            return FTAS_ERR_RECV;
        }
        recvd += (size_t)n;
    }
    return FTAS_OK;
}

static int send_frame(ftas_client_t *client, uint16_t function, const void *payload, uint16_t payload_len) {
    uint8_t frame[FTAS_MAX_MSG_SIZE];
    uint16_t length;

    if (!client || client->sockfd < 0) {
        return FTAS_ERR_PARAM;
    }
    if ((size_t)FTAS_HDR_SIZE + payload_len > sizeof(frame)) {
        return FTAS_ERR_PARAM;
    }

    length = (uint16_t)(FTAS_HDR_SIZE + payload_len);
    wr_le32(frame + 0, FTAS_SYNC_REQ);
    wr_le16(frame + 4, length);
    wr_le16(frame + 6, function);
    if (payload_len > 0 && payload) {
        memcpy(frame + FTAS_HDR_SIZE, payload, payload_len);
    }

    return write_all(client->sockfd, frame, length);
}

int ftas_client_connect(ftas_client_t *client, const char *ip, uint16_t port, int timeout_ms) {
    int fd;
    struct sockaddr_in addr;

    if (!client || !ip) {
        return FTAS_ERR_PARAM;
    }

    memset(client, 0, sizeof(*client));
    client->sockfd = -1;

    if (port == 0) {
        port = FTAS_DEFAULT_CONTROL_PORT;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return FTAS_ERR_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return FTAS_ERR_PARAM;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return FTAS_ERR_CONNECT;
    }

    client->sockfd = fd;
    client->timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    return set_socket_timeout_ms(fd, client->timeout_ms);
}

void ftas_client_close(ftas_client_t *client) {
    if (!client) {
        return;
    }
    if (client->sockfd >= 0) {
        close(client->sockfd);
    }
    client->sockfd = -1;
}

int ftas_get_version(ftas_client_t *client) {
    uint64_t reserved = 0;
    return send_frame(client, FTAS_FUNC_READ, &reserved, (uint16_t)sizeof(reserved));
}

int ftas_query_version(ftas_client_t *client, ftas_version_t *version) {
    ftas_control_msg_t msg;
    int rc;

    if (!version) {
        return FTAS_ERR_PARAM;
    }

    rc = ftas_get_version(client);
    if (rc != FTAS_OK) {
        return rc;
    }

    rc = ftas_recv_control_msg(client, &msg);
    if (rc != FTAS_OK) {
        return rc;
    }
    if (msg.function != FTAS_ACK_READ) {
        return FTAS_ERR_PROTO;
    }

    *version = msg.data.read;
    return FTAS_OK;
}

int ftas_set_bandwidth(ftas_client_t *client, uint64_t bw_mode) {
    return send_frame(client, FTAS_FUNC_BW_SET, &bw_mode, (uint16_t)sizeof(bw_mode));
}

int ftas_set_frequency_hz(ftas_client_t *client, uint64_t hz) {
    return send_frame(client, FTAS_FUNC_FREQ_SET, &hz, (uint16_t)sizeof(hz));
}

int ftas_set_gain(ftas_client_t *client, uint64_t gain) {
    return send_frame(client, FTAS_FUNC_GAIN_SET, &gain, (uint16_t)sizeof(gain));
}

int ftas_set_squelch(ftas_client_t *client, int64_t squelch_dbm) {
    return send_frame(client, FTAS_FUNC_SQL_SET, &squelch_dbm, (uint16_t)sizeof(squelch_dbm));
}

int ftas_set_code(ftas_client_t *client, int64_t code) {
    return send_frame(client, FTAS_FUNC_CODE_SET, &code, (uint16_t)sizeof(code));
}

int ftas_set_amplifier(ftas_client_t *client, int32_t onoff) {
    return send_frame(client, FTAS_FUNC_AMP_SET, &onoff, (uint16_t)sizeof(onoff));
}

int ftas_set_mode(ftas_client_t *client, int32_t mode) {
    return send_frame(client, FTAS_FUNC_MODE_SET, &mode, (uint16_t)sizeof(mode));
}

int ftas_set_tx_control_mode(ftas_client_t *client, int32_t mode) {
    return send_frame(client, FTAS_FUNC_TX_CTRL_MODE, &mode, (uint16_t)sizeof(mode));
}

int ftas_set_fm_am_mode(ftas_client_t *client, int32_t mode) {
    return send_frame(client, FTAS_FUNC_FM_AM_MODE, &mode, (uint16_t)sizeof(mode));
}

int ftas_clear_pscan_list(ftas_client_t *client) {
    return send_frame(client, FTAS_FUNC_PSCAN_CLEAR, NULL, 0);
}

int ftas_clear_buffer(ftas_client_t *client) {
    int32_t mode = 0;
    return send_frame(client, FTAS_FUNC_BUFFER_CLEAR, &mode, (uint16_t)sizeof(mode));
}

int ftas_start_record_iq(ftas_client_t *client, int32_t seconds) {
    return send_frame(client, FTAS_FUNC_START_RECORD_IQ, &seconds, (uint16_t)sizeof(seconds));
}

static int protocol_uses_default_user_channel(uint16_t protocol) {
    switch (protocol) {
    case 23:
    case 26:
    case 30:
    case 31:
    case 32:
    case 33:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
    case 48:
    case 51:
    case 52:
        return 1;
    default:
        return 0;
    }
}

void ftas_init_tx_param(ftas_tx_param_t *param, uint16_t protocol, int32_t mode) {
    if (!param) {
        return;
    }

    memset(param, 0, sizeof(*param));
    param->protocol = protocol;
    param->tx_id = FTAS_TX_PARAM_AUTO_FIELD;
    param->rx_id = FTAS_TX_PARAM_AUTO_FIELD;
    param->cc = FTAS_TX_PARAM_AUTO_FIELD;
    param->g_i = FTAS_TX_PARAM_AUTO_FIELD;
    param->vocoder = FTAS_TX_PARAM_AUTO_FIELD;
    param->tx_type = FTAS_TX_PARAM_AUTO_FIELD;
    param->channel_id = FTAS_TX_PARAM_AUTO_FIELD;
    param->user_id = 1;
    param->sequence_id = 1;
    param->mode = mode;
    param->reserved = FTAS_TX_PARAM_AUTO_FIELD;

    if (mode >= 1 && mode <= 3) {
        /* Keep tx_type aligned with action semantics for digital-cheat payload behavior. */
        param->tx_type = mode;
    }

    if (protocol_uses_default_user_channel(protocol)) {
        param->channel_id = 1;
    }

    switch (protocol) {
    case FTAS_PROTOCOL_DMR:
        param->tx_id = 1;
        param->rx_id = 1;
        param->cc = 1;
        param->g_i = 1;
        param->vocoder = 9371791;
        break;
    default:
        break;
    }
}

int ftas_send_tx_param(ftas_client_t *client, const ftas_tx_param_t *param) {
    uint8_t payload[FTAS_MAX_MSG_SIZE];
    size_t total;

    if (!client || !param) {
        return FTAS_ERR_PARAM;
    }
    if (param->data_len > (uint32_t)(FTAS_MAX_MSG_SIZE - FTAS_TX_PARAM_FIXED_PAYLOAD)) {
        return FTAS_ERR_PARAM;
    }
    if (param->data_len > 0 && !param->data_info) {
        return FTAS_ERR_PARAM;
    }

    wr_le16(payload + 0, param->protocol);
    wr_le32(payload + 2, (uint32_t)param->tx_id);
    wr_le32(payload + 6, (uint32_t)param->rx_id);
    wr_le32(payload + 10, (uint32_t)param->cc);
    wr_le32(payload + 14, (uint32_t)param->g_i);
    wr_le32(payload + 18, (uint32_t)param->vocoder);
    wr_le32(payload + 22, (uint32_t)param->tx_type);
    wr_le32(payload + 26, (uint32_t)param->channel_id);
    wr_le32(payload + 30, (uint32_t)param->user_id);
    wr_le32(payload + 34, (uint32_t)param->sequence_id);
    wr_le32(payload + 38, (uint32_t)param->mode);
    wr_le32(payload + 42, (uint32_t)param->reserved);
    wr_le32(payload + 46, param->data_len);

    if (param->data_len > 0) {
        memcpy(payload + FTAS_TX_PARAM_FIXED_PAYLOAD, param->data_info, param->data_len);
    }

    total = FTAS_TX_PARAM_FIXED_PAYLOAD + (size_t)param->data_len;
    return send_frame(client, FTAS_FUNC_TX_PARAM, payload, (uint16_t)total);
}

int ftas_tx_start(ftas_client_t *client, uint16_t repeat_mode, uint64_t frequency_hz,
                  uint16_t power, uint32_t duration_s) {
    uint8_t payload[18];

    wr_le16(payload + 0, 1);
    wr_le16(payload + 2, repeat_mode);
    wr_le64(payload + 4, frequency_hz);
    wr_le16(payload + 12, power);
    wr_le32(payload + 14, duration_s);

    return send_frame(client, FTAS_FUNC_TX_CONTROL, payload, (uint16_t)sizeof(payload));
}

int ftas_tx_stop(ftas_client_t *client) {
    uint8_t payload[18];

    memset(payload, 0, sizeof(payload));
    wr_le16(payload + 0, 2);

    return send_frame(client, FTAS_FUNC_TX_CONTROL, payload, (uint16_t)sizeof(payload));
}

int ftas_udp_open(ftas_udp_client_t *udp, uint16_t bind_port, int timeout_ms) {
    int fd;
    struct sockaddr_in addr;

    if (!udp) {
        return FTAS_ERR_PARAM;
    }

    memset(udp, 0, sizeof(*udp));
    udp->sockfd = -1;

    if (bind_port == 0) {
        bind_port = FTAS_DEFAULT_PARSE_PORT;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return FTAS_ERR_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bind_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return FTAS_ERR_SOCKET;
    }

    udp->sockfd = fd;
    udp->timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    return set_socket_timeout_ms(fd, udp->timeout_ms);
}

void ftas_udp_close(ftas_udp_client_t *udp) {
    if (!udp) {
        return;
    }
    if (udp->sockfd >= 0) {
        close(udp->sockfd);
    }
    udp->sockfd = -1;
}

int ftas_udp_recv(ftas_udp_client_t *udp, uint8_t *buf, size_t buf_cap, size_t *out_len) {
    ssize_t n;

    if (!udp || !buf || buf_cap == 0 || !out_len || udp->sockfd < 0) {
        return FTAS_ERR_PARAM;
    }

    n = recvfrom(udp->sockfd, buf, buf_cap, 0, NULL, NULL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return FTAS_ERR_TIMEOUT;
        }
        return FTAS_ERR_RECV;
    }

    *out_len = (size_t)n;
    return FTAS_OK;
}

int ftas_udp_decode_packet(const uint8_t *buf, size_t len, ftas_udp_packet_t *packet) {
    const uint8_t *body;
    size_t body_len;

    if (!buf || !packet || len < 16) {
        return FTAS_ERR_PARAM;
    }

    memset(packet, 0, sizeof(*packet));

    packet->header.sync = rd_le32(buf + 0);
    packet->header.length = rd_le16(buf + 4);
    packet->header.minor_version = rd_le16(buf + 6);
    packet->header.major_version = rd_le16(buf + 8);
    packet->header.head_type = rd_le16(buf + 10);
    packet->header.power = (int32_t)rd_le32(buf + 12);

    if (packet->header.sync != FTAS_SYNC_RESP) {
        return FTAS_ERR_PROTO;
    }
    if (packet->header.length != 16) {
        return FTAS_ERR_PROTO;
    }
    body = buf + 16;
    body_len = len - 16;
    if (body_len < 6) {
        return FTAS_ERR_PROTO;
    }

    packet->raw_body = body;
    packet->raw_body_len = (uint32_t)body_len;

    packet->result.body_type = rd_le16(body + 0);
    packet->result.body_length = rd_le32(body + 2);
    packet->jam_iq.body_type = packet->result.body_type;
    packet->jam_iq.body_length = packet->result.body_length;

    if (packet->result.body_type == 201 || packet->result.body_type == 203 ||
        packet->result.body_type == 204) {
        if (body_len < 14) {
            return FTAS_ERR_PROTO;
        }

        packet->result.signal_type = rd_le16(body + 6);
        packet->result.signal_freq = rd_le32(body + 8);
        packet->result.signal_body_len = rd_le16(body + 12);
        if (body_len < (size_t)(14 + packet->result.signal_body_len)) {
            return FTAS_ERR_PROTO;
        }
        packet->result.signal_body = body + 14;
        return FTAS_OK;
    }

    if (packet->result.body_type == 202) {
        if (body_len < 22) {
            return FTAS_ERR_PROTO;
        }
        packet->jam_iq.sample_rate = rd_le32(body + 6);
        packet->jam_iq.bandwidth = rd_le32(body + 10);
        packet->jam_iq.sample_count = (uint64_t)rd_le32(body + 14) | ((uint64_t)rd_le32(body + 18) << 32);
        packet->jam_iq.iq_data = (const int16_t *)(const void *)(body + 22);
        packet->jam_iq.iq_short_count = (body_len - 22) / 2;
        return FTAS_OK;
    }

    /* Unknown body type: caller can consume raw_body/raw_body_len. */
    return FTAS_OK;
}

int ftas_iq_sender_open(ftas_iq_sender_t *sender,
                        const char *ip,
                        uint16_t port,
                        int timeout_ms,
                        uint16_t major_version,
                        uint16_t minor_version) {
    int fd;
    struct sockaddr_in addr;

    if (!sender || !ip) {
        return FTAS_ERR_PARAM;
    }

    memset(sender, 0, sizeof(*sender));
    sender->sockfd = -1;

    if (port == 0) {
        port = FTAS_DEFAULT_IQ_INPUT_PORT;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return FTAS_ERR_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return FTAS_ERR_PARAM;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return FTAS_ERR_CONNECT;
    }

    sender->sockfd = fd;
    sender->timeout_ms = timeout_ms > 0 ? timeout_ms : 3000;
    sender->major_version = major_version;
    sender->minor_version = minor_version;
    sender->sequence = 0;
    sender->sample_count = 0;

    return set_socket_timeout_ms(fd, sender->timeout_ms);
}

void ftas_iq_sender_close(ftas_iq_sender_t *sender) {
    if (!sender) {
        return;
    }
    if (sender->sockfd >= 0) {
        close(sender->sockfd);
    }
    sender->sockfd = -1;
}

int ftas_iq_sender_send_interleaved(ftas_iq_sender_t *sender,
                                    const ftas_iq_meta_t *meta,
                                    const int16_t *iq_interleaved,
                                    uint32_t iq_pairs) {
    uint8_t *pkt;
    size_t pkt_len;
    size_t iq_bytes;
    uint16_t major;
    uint16_t minor;
    uint16_t body_type;
    int rc;

    if (!sender || !meta || !iq_interleaved || sender->sockfd < 0 || iq_pairs == 0) {
        return FTAS_ERR_PARAM;
    }

    iq_bytes = (size_t)iq_pairs * 4;
    pkt_len = 16 + FTAS_IQ_FIXED_BODY_BYTES + iq_bytes;

    pkt = (uint8_t *)malloc(pkt_len);
    if (!pkt) {
        return FTAS_ERR_PARAM;
    }

    major = meta->major_version ? meta->major_version : sender->major_version;
    minor = meta->minor_version ? meta->minor_version : sender->minor_version;
    body_type = meta->body_type;

    wr_le32(pkt + 0, FTAS_SYNC_REQ);
    wr_le16(pkt + 4, 16);
    wr_le16(pkt + 6, minor);
    wr_le16(pkt + 8, major);
    wr_le32(pkt + 10, sender->sequence);
    wr_le16(pkt + 14, 0);

    wr_le16(pkt + 16, body_type);
    wr_le32(pkt + 18, (uint32_t)(FTAS_IQ_FIXED_BODY_BYTES + iq_bytes));
    wr_le32(pkt + 22, meta->sample_rate);
    wr_le64(pkt + 26, meta->frequency_hz);
    wr_le32(pkt + 34, meta->bandwidth);
    wr_le64(pkt + 38, sender->sample_count);

    memcpy(pkt + 46, iq_interleaved, iq_bytes);

    rc = write_all(sender->sockfd, pkt, pkt_len);
    free(pkt);
    if (rc != FTAS_OK) {
        return rc;
    }

    sender->sequence += 1;
    sender->sample_count += (uint64_t)iq_pairs;
    return FTAS_OK;
}

int ftas_iq_sender_send_zero(ftas_iq_sender_t *sender,
                             const ftas_iq_meta_t *meta,
                             uint32_t iq_pairs) {
    int16_t *buf;
    int rc;

    if (!sender || !meta || iq_pairs == 0) {
        return FTAS_ERR_PARAM;
    }

    buf = (int16_t *)calloc((size_t)iq_pairs * 2, sizeof(int16_t));
    if (!buf) {
        return FTAS_ERR_PARAM;
    }

    rc = ftas_iq_sender_send_interleaved(sender, meta, buf, iq_pairs);
    free(buf);
    return rc;
}

int ftas_recv_control_msg(ftas_client_t *client, ftas_control_msg_t *msg) {
    uint8_t hdr[FTAS_HDR_SIZE];
    uint8_t body[FTAS_MAX_MSG_SIZE];
    uint32_t sync;
    uint16_t length;
    uint16_t function;
    uint16_t body_len;
    int rc;

    if (!client || !msg || client->sockfd < 0) {
        return FTAS_ERR_PARAM;
    }

    rc = read_all(client->sockfd, hdr, sizeof(hdr));
    if (rc != FTAS_OK) {
        return rc;
    }

    sync = rd_le32(hdr + 0);
    length = rd_le16(hdr + 4);
    function = rd_le16(hdr + 6);

    if (sync != FTAS_SYNC_RESP || length < FTAS_HDR_SIZE) {
        return FTAS_ERR_PROTO;
    }

    body_len = (uint16_t)(length - FTAS_HDR_SIZE);
    if (body_len > sizeof(body)) {
        return FTAS_ERR_PROTO;
    }

    if (body_len > 0) {
        rc = read_all(client->sockfd, body, body_len);
        if (rc != FTAS_OK) {
            return rc;
        }
    }

    memset(msg, 0, sizeof(*msg));
    msg->function = function;
    msg->length = length;

    if (function == FTAS_ACK_READ) {
        if (body_len < 10) {
            return FTAS_ERR_PROTO;
        }
        msg->data.read.hardware = rd_le16(body + 0);
        msg->data.read.firmware = rd_le16(body + 2);
        msg->data.read.analyser = rd_le16(body + 4);
        msg->data.read.generator = rd_le16(body + 6);
        msg->data.read.protocol = rd_le16(body + 8);
    } else if (function == FTAS_ACK_STATE) {
        if (body_len < 2) {
            return FTAS_ERR_PROTO;
        }
        msg->data.state.state = rd_le16(body + 0);
    } else if (function == FTAS_ACK_TX_STATE) {
        if (body_len < 5) {
            return FTAS_ERR_PROTO;
        }
        msg->data.tx_state.state = (int8_t)body[0];
        msg->data.tx_state.message_len = rd_le32(body + 1);

        if (msg->data.tx_state.message_len > 0) {
            size_t copy_len = msg->data.tx_state.message_len;
            if (copy_len > sizeof(msg->data.tx_state.message) - 1) {
                copy_len = sizeof(msg->data.tx_state.message) - 1;
            }
            if (copy_len > (size_t)(body_len - 5)) {
                copy_len = (size_t)(body_len - 5);
            }
            memcpy(msg->data.tx_state.message, body + 5, copy_len);
            msg->data.tx_state.message[copy_len] = '\0';
        }
    }

    return FTAS_OK;
}

int ftas_wait_state_ack(ftas_client_t *client, ftas_state_ack_t *ack) {
    ftas_control_msg_t msg;
    int rc;

    if (!ack) {
        return FTAS_ERR_PARAM;
    }

    rc = ftas_recv_control_msg(client, &msg);
    if (rc != FTAS_OK) {
        return rc;
    }
    if (msg.function != FTAS_ACK_STATE) {
        return FTAS_ERR_PROTO;
    }

    *ack = msg.data.state;
    return FTAS_OK;
}

int ftas_wait_tx_state_ack(ftas_client_t *client, ftas_tx_state_ack_t *ack) {
    ftas_control_msg_t msg;
    int rc;

    if (!ack) {
        return FTAS_ERR_PARAM;
    }

    rc = ftas_recv_control_msg(client, &msg);
    if (rc != FTAS_OK) {
        return rc;
    }
    if (msg.function != FTAS_ACK_TX_STATE) {
        return FTAS_ERR_PROTO;
    }

    *ack = msg.data.tx_state;
    return FTAS_OK;
}

static void *ftas_udp_loop_thread(void *arg) {
    ftas_threadsafe_t *ctx = (ftas_threadsafe_t *)arg;

    while (ctx->running) {
        uint8_t buf[65535];
        size_t n = 0;
        ftas_udp_packet_t pkt;
        int rc;

        rc = ftas_udp_recv(&ctx->udp, buf, sizeof(buf), &n);
        if (rc == FTAS_ERR_TIMEOUT) {
            continue;
        }
        if (rc != FTAS_OK) {
            continue;
        }

        rc = ftas_udp_decode_packet(buf, n, &pkt);
        if (rc != FTAS_OK) {
            continue;
        }

        if (ctx->udp_cb) {
            ctx->udp_cb(&pkt, buf, n, ctx->udp_cb_user);
        }
    }

    return NULL;
}

int ftas_ts_open(ftas_threadsafe_t *ctx,
                 const char *ip,
                 uint16_t control_port,
                 uint16_t udp_bind_port,
                 int timeout_ms) {
    int rc;

    if (!ctx || !ip) {
        return FTAS_ERR_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->control.sockfd = -1;
    ctx->udp.sockfd = -1;

    rc = pthread_mutex_init(&ctx->control_lock, NULL);
    if (rc != 0) {
        return FTAS_ERR_SOCKET;
    }

    rc = ftas_client_connect(&ctx->control, ip, control_port, timeout_ms);
    if (rc != FTAS_OK) {
        pthread_mutex_destroy(&ctx->control_lock);
        return rc;
    }

    rc = ftas_udp_open(&ctx->udp, udp_bind_port, timeout_ms);
    if (rc != FTAS_OK) {
        ftas_client_close(&ctx->control);
        pthread_mutex_destroy(&ctx->control_lock);
        return rc;
    }

    ctx->udp_opened = 1;
    ctx->running = 0;
    return FTAS_OK;
}

void ftas_ts_stop_udp_loop(ftas_threadsafe_t *ctx) {
    if (!ctx) {
        return;
    }
    if (!ctx->running) {
        return;
    }

    ctx->running = 0;
    pthread_join(ctx->udp_thread, NULL);
}

void ftas_ts_close(ftas_threadsafe_t *ctx) {
    if (!ctx) {
        return;
    }

    ftas_ts_stop_udp_loop(ctx);
    if (ctx->udp_opened) {
        ftas_udp_close(&ctx->udp);
        ctx->udp_opened = 0;
    }
    ftas_client_close(&ctx->control);
    pthread_mutex_destroy(&ctx->control_lock);
}

int ftas_ts_start_udp_loop(ftas_threadsafe_t *ctx, ftas_udp_packet_cb cb, void *user) {
    int rc;

    if (!ctx || !cb || !ctx->udp_opened) {
        return FTAS_ERR_PARAM;
    }
    if (ctx->running) {
        return FTAS_ERR_PARAM;
    }

    ctx->udp_cb = cb;
    ctx->udp_cb_user = user;
    ctx->running = 1;

    rc = pthread_create(&ctx->udp_thread, NULL, ftas_udp_loop_thread, ctx);
    if (rc != 0) {
        ctx->running = 0;
        return FTAS_ERR_SOCKET;
    }

    return FTAS_OK;
}

int ftas_ts_query_version(ftas_threadsafe_t *ctx, ftas_version_t *version) {
    int rc;
    if (!ctx || !version) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_query_version(&ctx->control, version);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_set_frequency_hz(ftas_threadsafe_t *ctx, uint64_t hz) {
    int rc;
    if (!ctx) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_set_frequency_hz(&ctx->control, hz);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_set_gain(ftas_threadsafe_t *ctx, uint64_t gain) {
    int rc;
    if (!ctx) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_set_gain(&ctx->control, gain);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_set_bandwidth(ftas_threadsafe_t *ctx, uint64_t bw_mode) {
    int rc;
    if (!ctx) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_set_bandwidth(&ctx->control, bw_mode);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_send_tx_param(ftas_threadsafe_t *ctx, const ftas_tx_param_t *param) {
    int rc;
    if (!ctx || !param) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_send_tx_param(&ctx->control, param);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_tx_start(ftas_threadsafe_t *ctx,
                     uint16_t repeat_mode,
                     uint64_t frequency_hz,
                     uint16_t power,
                     uint32_t duration_s) {
    int rc;
    if (!ctx) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_tx_start(&ctx->control, repeat_mode, frequency_hz, power, duration_s);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

int ftas_ts_tx_stop(ftas_threadsafe_t *ctx) {
    int rc;
    if (!ctx) {
        return FTAS_ERR_PARAM;
    }
    pthread_mutex_lock(&ctx->control_lock);
    rc = ftas_tx_stop(&ctx->control);
    pthread_mutex_unlock(&ctx->control_lock);
    return rc;
}

const char *ftas_strerror(int code) {
    switch (code) {
        case FTAS_OK:
            return "ok";
        case FTAS_ERR_PARAM:
            return "invalid parameter";
        case FTAS_ERR_SOCKET:
            return "socket error";
        case FTAS_ERR_CONNECT:
            return "connect failed";
        case FTAS_ERR_SEND:
            return "send failed";
        case FTAS_ERR_RECV:
            return "recv failed";
        case FTAS_ERR_PROTO:
            return "protocol parse error";
        case FTAS_ERR_TIMEOUT:
            return "io timeout";
        default:
            return "unknown error";
    }
}
