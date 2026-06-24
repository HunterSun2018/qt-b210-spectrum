#ifndef FTAS_SDK_H
#define FTAS_SDK_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTAS_DEFAULT_CONTROL_PORT 10110
#define FTAS_DEFAULT_PARSE_PORT 10111
#define FTAS_DEFAULT_IQ_INPUT_PORT 10112
#define FTAS_SYNC_REQ  0x000000FFu
#define FTAS_SYNC_RESP 0xFFFFFFFFu

typedef enum {
    FTAS_OK = 0,
    FTAS_ERR_PARAM = -1,
    FTAS_ERR_SOCKET = -2,
    FTAS_ERR_CONNECT = -3,
    FTAS_ERR_SEND = -4,
    FTAS_ERR_RECV = -5,
    FTAS_ERR_PROTO = -6,
    FTAS_ERR_TIMEOUT = -7
} ftas_status_t;

typedef enum {
    FTAS_FUNC_READ = 0x11,
    FTAS_FUNC_BW_SET = 0x12,
    FTAS_FUNC_FREQ_SET = 0x13,
    FTAS_FUNC_GAIN_SET = 0x14,
    FTAS_FUNC_SQL_SET = 0x15,
    FTAS_FUNC_CODE_SET = 0x16,
    FTAS_FUNC_TX_PARAM = 0x17,
    FTAS_FUNC_TX_CONTROL = 0x18,
    FTAS_FUNC_AMP_SET = 0x1B,
    FTAS_FUNC_MODE_SET = 0x1C,
    FTAS_FUNC_TX_CTRL_MODE = 0x1D,
    FTAS_FUNC_FM_AM_MODE = 0x1E,
    FTAS_FUNC_BUFFER_CLEAR = 0x1F,
    FTAS_FUNC_PSCAN_CLEAR = 0x20,
    FTAS_FUNC_START_RECORD_IQ = 0x21
} ftas_function_t;

typedef enum {
    FTAS_ACK_READ = 0x04,
    FTAS_ACK_STATE = 0x05,
    FTAS_ACK_TX_STATE = 0x06
} ftas_ack_function_t;

typedef struct {
    int sockfd;
    int timeout_ms;
} ftas_client_t;

typedef struct {
    int sockfd;
    int timeout_ms;
} ftas_udp_client_t;

typedef struct {
    int sockfd;
    int timeout_ms;
    uint16_t minor_version;
    uint16_t major_version;
    uint32_t sequence;
    uint64_t sample_count;
} ftas_iq_sender_t;

typedef struct {
    uint32_t sample_rate;
    uint64_t frequency_hz;
    uint32_t bandwidth;
    uint16_t minor_version;
    uint16_t major_version;
    uint16_t body_type;
} ftas_iq_meta_t;

typedef enum {
    FTAS_HEAD_RESULT = 201,
    FTAS_HEAD_JAM_IQ = 202,
    FTAS_HEAD_MODULATION = 203,
    FTAS_HEAD_DEMOD_PARAM = 204
} ftas_parse_head_type_t;

typedef struct {
    uint16_t protocol;
    int32_t tx_id;
    int32_t rx_id;
    int32_t cc;
    int32_t g_i;
    int32_t vocoder;
    int32_t tx_type;
    int32_t channel_id;
    int32_t user_id;
    int32_t sequence_id;
    int32_t mode;
    int32_t reserved;
    const uint8_t *data_info;
    uint32_t data_len;
} ftas_tx_param_t;

#define FTAS_TX_PARAM_AUTO_FIELD ((int32_t)0x008F008F)

enum {
    FTAS_PROTOCOL_DMR = 12,
    FTAS_PROTOCOL_CLOUD5 = 23,
    FTAS_PROTOCOL_CLOUD6 = 41
};

typedef struct {
    uint32_t sync;
    uint16_t length;
    uint16_t minor_version;
    uint16_t major_version;
    uint16_t head_type;
    int32_t power;
} ftas_parse_header_t;

typedef struct {
    uint16_t body_type;
    uint32_t body_length;
    uint16_t signal_type;
    uint32_t signal_freq;
    uint16_t signal_body_len;
    const uint8_t *signal_body;
} ftas_result_body_t;

typedef struct {
    uint16_t body_type;
    uint32_t body_length;
    uint32_t sample_rate;
    uint32_t bandwidth;
    uint64_t sample_count;
    const int16_t *iq_data;
    size_t iq_short_count;
} ftas_jam_iq_body_t;

typedef struct {
    ftas_parse_header_t header;
    uint32_t raw_body_len;
    const uint8_t *raw_body;
    ftas_result_body_t result;
    ftas_jam_iq_body_t jam_iq;
} ftas_udp_packet_t;

typedef void (*ftas_udp_packet_cb)(const ftas_udp_packet_t *packet,
                                   const uint8_t *raw_buf,
                                   size_t raw_len,
                                   void *user);

typedef struct {
    ftas_client_t control;
    ftas_udp_client_t udp;
    pthread_mutex_t control_lock;
    pthread_t udp_thread;
    int running;
    int udp_opened;
    ftas_udp_packet_cb udp_cb;
    void *udp_cb_user;
} ftas_threadsafe_t;

typedef struct {
    uint16_t hardware;
    uint16_t firmware;
    uint16_t analyser;
    uint16_t generator;
    uint16_t protocol;
} ftas_version_t;

typedef struct {
    uint16_t state;
} ftas_state_ack_t;

typedef struct {
    int8_t state;
    uint32_t message_len;
    char message[256];
} ftas_tx_state_ack_t;

typedef struct {
    uint16_t function;
    uint16_t length;
    union {
        ftas_version_t read;
        ftas_state_ack_t state;
        ftas_tx_state_ack_t tx_state;
    } data;
} ftas_control_msg_t;

int ftas_client_connect(ftas_client_t *client, const char *ip, uint16_t port, int timeout_ms);
void ftas_client_close(ftas_client_t *client);

int ftas_get_version(ftas_client_t *client);
int ftas_query_version(ftas_client_t *client, ftas_version_t *version);
int ftas_set_bandwidth(ftas_client_t *client, uint64_t bw_mode);
int ftas_set_frequency_hz(ftas_client_t *client, uint64_t hz);
int ftas_set_gain(ftas_client_t *client, uint64_t gain);
int ftas_set_squelch(ftas_client_t *client, int64_t squelch_dbm);
int ftas_set_code(ftas_client_t *client, int64_t code);
int ftas_set_amplifier(ftas_client_t *client, int32_t onoff);
int ftas_set_mode(ftas_client_t *client, int32_t mode);
int ftas_set_tx_control_mode(ftas_client_t *client, int32_t mode);
int ftas_set_fm_am_mode(ftas_client_t *client, int32_t mode);
int ftas_clear_pscan_list(ftas_client_t *client);
int ftas_clear_buffer(ftas_client_t *client);
int ftas_start_record_iq(ftas_client_t *client, int32_t seconds);
void ftas_init_tx_param(ftas_tx_param_t *param, uint16_t protocol, int32_t mode);
int ftas_send_tx_param(ftas_client_t *client, const ftas_tx_param_t *param);
int ftas_tx_start(ftas_client_t *client, uint16_t repeat_mode, uint64_t frequency_hz,
                  uint16_t power, uint32_t duration_s);
int ftas_tx_stop(ftas_client_t *client);

int ftas_udp_open(ftas_udp_client_t *udp, uint16_t bind_port, int timeout_ms);
void ftas_udp_close(ftas_udp_client_t *udp);
int ftas_udp_recv(ftas_udp_client_t *udp, uint8_t *buf, size_t buf_cap, size_t *out_len);
int ftas_udp_decode_packet(const uint8_t *buf, size_t len, ftas_udp_packet_t *packet);

int ftas_iq_sender_open(ftas_iq_sender_t *sender,
                        const char *ip,
                        uint16_t port,
                        int timeout_ms,
                        uint16_t major_version,
                        uint16_t minor_version);
void ftas_iq_sender_close(ftas_iq_sender_t *sender);
int ftas_iq_sender_send_interleaved(ftas_iq_sender_t *sender,
                                    const ftas_iq_meta_t *meta,
                                    const int16_t *iq_interleaved,
                                    uint32_t iq_pairs);
int ftas_iq_sender_send_zero(ftas_iq_sender_t *sender,
                             const ftas_iq_meta_t *meta,
                             uint32_t iq_pairs);

int ftas_recv_control_msg(ftas_client_t *client, ftas_control_msg_t *msg);
int ftas_wait_state_ack(ftas_client_t *client, ftas_state_ack_t *ack);
int ftas_wait_tx_state_ack(ftas_client_t *client, ftas_tx_state_ack_t *ack);

int ftas_ts_open(ftas_threadsafe_t *ctx,
                 const char *ip,
                 uint16_t control_port,
                 uint16_t udp_bind_port,
                 int timeout_ms);
void ftas_ts_close(ftas_threadsafe_t *ctx);
int ftas_ts_start_udp_loop(ftas_threadsafe_t *ctx, ftas_udp_packet_cb cb, void *user);
void ftas_ts_stop_udp_loop(ftas_threadsafe_t *ctx);

int ftas_ts_query_version(ftas_threadsafe_t *ctx, ftas_version_t *version);
int ftas_ts_set_frequency_hz(ftas_threadsafe_t *ctx, uint64_t hz);
int ftas_ts_set_gain(ftas_threadsafe_t *ctx, uint64_t gain);
int ftas_ts_set_bandwidth(ftas_threadsafe_t *ctx, uint64_t bw_mode);
int ftas_ts_send_tx_param(ftas_threadsafe_t *ctx, const ftas_tx_param_t *param);
int ftas_ts_tx_start(ftas_threadsafe_t *ctx,
                     uint16_t repeat_mode,
                     uint64_t frequency_hz,
                     uint16_t power,
                     uint32_t duration_s);
int ftas_ts_tx_stop(ftas_threadsafe_t *ctx);
const char *ftas_strerror(int code);

#ifdef __cplusplus
}
#endif

#endif
