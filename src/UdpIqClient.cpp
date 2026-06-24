#include "UdpIqClient.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <iostream>
#include <string>
#include <codecvt>
#include <locale>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ftas_sdk.h>

namespace
{
    constexpr std::uint32_t kFtasSyncReq = 0x000000FFu;
    constexpr std::uint32_t kFtasSyncResp = 0xFFFFFFFFu;
    constexpr std::size_t kPacketHeaderBytes = 16;
    constexpr std::size_t kIqFixedBodyBytes = 30;
    constexpr std::size_t kMaxPacketBytes = 65535;

    std::uint16_t readLe16(const std::uint8_t *data)
    {
        return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8));
    }

    std::uint32_t readLe32(const std::uint8_t *data)
    {
        return static_cast<std::uint32_t>(data[0]) |
               (static_cast<std::uint32_t>(data[1]) << 8) |
               (static_cast<std::uint32_t>(data[2]) << 16) |
               (static_cast<std::uint32_t>(data[3]) << 24);
    }

    std::uint64_t readLe64(const std::uint8_t *data)
    {
        return static_cast<std::uint64_t>(readLe32(data)) |
               (static_cast<std::uint64_t>(readLe32(data + 4)) << 32);
    }

    void writeLe16(std::uint8_t *data, std::uint16_t value)
    {
        data[0] = static_cast<std::uint8_t>(value & 0xffU);
        data[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    }

    void writeLe32(std::uint8_t *data, std::uint32_t value)
    {
        data[0] = static_cast<std::uint8_t>(value & 0xffU);
        data[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        data[2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
        data[3] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    }

    void writeLe64(std::uint8_t *data, std::uint64_t value)
    {
        writeLe32(data, static_cast<std::uint32_t>(value & 0xffffffffULL));
        writeLe32(data + 4, static_cast<std::uint32_t>(value >> 32));
    }

    sockaddr_in makeSockaddr(const std::string &address, std::uint16_t port)
    {
        sockaddr_in sockaddr{};
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        if (::inet_pton(AF_INET, address.c_str(), &sockaddr.sin_addr) != 1)
        {
            throw std::runtime_error("Invalid IPv4 address: " + address);
        }
        return sockaddr;
    }

    UdpIqClient::ResponsePacket decodePacket(const std::vector<std::uint8_t> &buffer)
    {
        if (buffer.size() < kPacketHeaderBytes)
        {
            throw std::runtime_error("UDP response too short");
        }

        UdpIqClient::ResponsePacket packet;
        packet.header.sync = readLe32(buffer.data() + 0);
        packet.header.length = readLe16(buffer.data() + 4);
        packet.header.minorVersion = readLe16(buffer.data() + 6);
        packet.header.majorVersion = readLe16(buffer.data() + 8);
        packet.header.headType = readLe16(buffer.data() + 10);
        packet.header.power = static_cast<std::int32_t>(readLe32(buffer.data() + 12));

        if (packet.header.sync != kFtasSyncResp)
        {
            throw std::runtime_error("Unexpected UDP response sync");
        }
        if (packet.header.length != kPacketHeaderBytes)
        {
            throw std::runtime_error("Unexpected UDP response header length");
        }

        packet.rawBody.assign(buffer.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderBytes), buffer.end());
        if (packet.rawBody.size() < 6)
        {
            throw std::runtime_error("UDP response body too short");
        }

        const std::uint8_t *body = packet.rawBody.data();
        const std::size_t bodyLen = packet.rawBody.size();

        packet.result.bodyType = readLe16(body + 0);
        packet.result.bodyLength = readLe32(body + 2);
        packet.jamIq.bodyType = packet.result.bodyType;
        packet.jamIq.bodyLength = packet.result.bodyLength;

        if (packet.result.bodyType == 201 || packet.result.bodyType == 203 || packet.result.bodyType == 204)
        {
            if (bodyLen < 14)
            {
                throw std::runtime_error("UDP result packet too short");
            }

            packet.result.signalType = readLe16(body + 6);
            packet.result.signalFreq = readLe32(body + 8);
            packet.result.signalBodyLength = readLe16(body + 12);
            if (bodyLen < static_cast<std::size_t>(14 + packet.result.signalBodyLength))
            {
                throw std::runtime_error("UDP result packet payload truncated");
            }

            packet.result.signalBody.assign(body + 14, body + 14 + packet.result.signalBodyLength);
            return packet;
        }

        if (packet.result.bodyType == 202)
        {
            if (bodyLen < 22)
            {
                throw std::runtime_error("UDP IQ packet too short");
            }

            packet.jamIq.sampleRate = readLe32(body + 6);
            packet.jamIq.bandwidth = readLe32(body + 10);
            packet.jamIq.sampleCount = readLe64(body + 14);

            const std::size_t iqPairCount = (bodyLen - 22) / 4;
            packet.jamIq.iqSamples.resize(iqPairCount);
            for (std::size_t index = 0; index < iqPairCount; ++index)
            {
                const std::uint8_t *iq = body + 22 + (index * 4);
                packet.jamIq.iqSamples[index] = std::complex<std::int16_t>(
                    static_cast<std::int16_t>(readLe16(iq)),
                    static_cast<std::int16_t>(readLe16(iq + 2)));
            }
        }

        return packet;
    }

    std::string to_utf8(std::u16string str16)
    {
        return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(str16);
    }

}

UdpIqClient::UdpIqClient() = default;

UdpIqClient::~UdpIqClient()
{
    close();
}

void UdpIqClient::parser_ftas_control_init(const char *ftas_ip,
                                           uint16_t control_port,
                                           int timeout_ms)
{
    ftas_version_t version;
    uint64_t center_freq_hz;
    uint32_t gain;
    int rc;

    rc = ftas_client_connect(&m_ftas_client, ftas_ip, control_port, timeout_ms);
    if (rc != FTAS_OK)
    {

        fprintf(stderr,
                "%s: parser FTAS control connect warning ip=%s port=%u: %s\n",
                "ftasd-parser",
                ftas_ip ? ftas_ip : "-",
                (unsigned int)control_port,
                ftas_strerror(rc));
        return;
    }

    rc = ftas_query_version(&m_ftas_client, &version);
    if (rc == FTAS_OK)
    {
        fprintf(stderr,
                "%s: parser FTAS version hw=%u fw=%u analyser=%u gen=%u proto=%u\n",
                "ftasd-parser",
                (unsigned int)version.hardware,
                (unsigned int)version.firmware,
                (unsigned int)version.analyser,
                (unsigned int)version.generator,
                (unsigned int)version.protocol);
    }
    else
    {
        fprintf(stderr,
                "%s: parser FTAS query version warning: %s\n",
                "ftasd-parser",
                ftas_strerror(rc));
    }

    rc = ftas_clear_buffer(&m_ftas_client);
    if (rc != FTAS_OK)
    {
        fprintf(stderr,
                "%s: parser FTAS clear buffer warning: %s\n",
                "ftasd-parser",
                ftas_strerror(rc));
    }
    rc = ftas_clear_pscan_list(&m_ftas_client);
    if (rc != FTAS_OK)
    {
        fprintf(stderr,
                "%s: parser FTAS clear pscan warning: %s\n",
                "ftasd-parser",
                ftas_strerror(rc));
    }

    center_freq_hz = 172700000;
    if (center_freq_hz != 0u)
    {
        rc = ftas_set_frequency_hz(&m_ftas_client, center_freq_hz);
        if (rc != FTAS_OK)
        {
            fprintf(stderr,
                    "%s: parser FTAS set frequency warning center=%llu: %s\n",
                    "ftasd-parser",
                    (unsigned long long)center_freq_hz,
                    ftas_strerror(rc));
        }
    }

    gain = 10;
    rc = ftas_set_gain(&m_ftas_client, gain);
    if (rc != FTAS_OK)
    {
        fprintf(stderr,
                "%s: parser FTAS set gain warning gain=%u: %s\n",
                "ftasd-parser",
                (unsigned int)gain,
                ftas_strerror(rc));
    }

    rc = ftas_set_mode(&m_ftas_client, 2);
    if (rc != FTAS_OK)
    {
        fprintf(stderr,
                "%s: parser FTAS set remote mode warning: %s\n",
                "ftasd-parser",
                ftas_strerror(rc));
    }
    else
    {
        fprintf(stderr,
                "%s: parser FTAS control initialized port=%u center=%llu gain=%u mode=remote keepalive=1\n",
                "ftasd-parser",
                (unsigned int)control_port,
                (unsigned long long)center_freq_hz,
                (unsigned int)gain);
    }
}

void UdpIqClient::open(const Endpoint &remote, const BindEndpoint &local)
{
    const int timeout_ms = 3000;

    close();

    parser_ftas_control_init("192.168.1.50", 10110, timeout_ms);

    int rc = ftas_udp_open(&m_ftas_udp_client, 10111, timeout_ms);

    rc = ftas_iq_sender_open(&m_ftas_iq_sender, remote.address.c_str(), remote.port, timeout_ms, 0u, 0u);

    try
    {
        // m_socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        // if (m_socketFd < 0)
        // {
        //     throw std::system_error(errno, std::generic_category(), "Failed to create UDP socket");
        // }

        // m_socketRx = ::socket(AF_INET, SOCK_DGRAM, 0);
        // if (m_socketRx < 0)
        // {
        //     throw std::system_error(errno, std::generic_category(), "Failed to create receiver UDP socket");
        // }

        // const sockaddr_in localAddr = makeSockaddr(local.address, local.port);
        // if (::bind(m_socketFd, reinterpret_cast<const sockaddr *>(&localAddr), sizeof(localAddr)) != 0)
        // {
        //     throw std::system_error(errno, std::generic_category(), "Failed to bind UDP socket");
        // }

        // const sockaddr_in remoteAddr = makeSockaddr(remote.address, remote.port);
        // if (::connect(m_socketFd, reinterpret_cast<const sockaddr *>(&remoteAddr), sizeof(remoteAddr)) != 0)
        // {
        //     throw std::system_error(errno, std::generic_category(), "Failed to connect UDP socket");
        // }

        // const sockaddr_in localAddr1 = makeSockaddr(local.address, 10111);
        // if (::bind(m_socketRx, reinterpret_cast<const sockaddr *>(&localAddr1), sizeof(localAddr1)) != 0)
        // {
        //     throw std::system_error(errno, std::generic_category(), "Failed to bind recever UDP socket");
        // }
    }
    catch (...)
    {
        close();
        throw;
    }
}

void UdpIqClient::close()
{
    if (m_socketFd >= 0)
    {
        ::close(m_socketFd);
        m_socketFd = -1;
    }

    m_sequence = 0;
    m_sampleCount = 0;
}

bool UdpIqClient::isOpen() const
{
    return m_socketFd >= 0;
}

void UdpIqClient::sendInterleaved(const SendMeta &sendMeta,
                                  const std::complex<std::int16_t> *samples,
                                  std::uint32_t iqPairs)
{
    // if (!isOpen())
    // {
    //     throw std::runtime_error("UDP socket is not open");
    // }
    if (samples == nullptr || iqPairs == 0)
    {
        throw std::runtime_error("No IQ samples provided for UDP send");
    }

    ftas_iq_meta_t meta;

    memset(&meta, 0, sizeof(meta));
    meta.sample_rate = sendMeta.sampleRate;
    meta.frequency_hz = sendMeta.frequencyHz;
    meta.bandwidth = sendMeta.bandwidth;

    int rc = ftas_iq_sender_send_interleaved(&m_ftas_iq_sender,
                                             &meta,
                                             (const int16_t *)samples,
                                             iqPairs);
    if (rc != FTAS_OK)
    {
        std::cout << "sending IQ error" << std::endl;
    }

    // for (size_t i = 0; i <= iqPairs / 8192; i++)
    // {
    //     int rc = ftas_iq_sender_send_interleaved(&m_ftas_iq_sender,
    //                                              &meta,
    //                                              (const int16_t *)samples + i*8192,
    //                                              8192);
    //     if (rc != FTAS_OK)
    //     {
    //         std::cout << "sending IQ error" << std::endl;
    //     }
    // }

    // const std::size_t iqBytes = static_cast<std::size_t>(iqPairs) * 4U;
    // std::vector<std::uint8_t> packet(kPacketHeaderBytes + kIqFixedBodyBytes + iqBytes);

    // const std::uint16_t majorVersion = meta.majorVersion; // != 0 ? meta.majorVersion : m_majorVersion;
    // const std::uint16_t minorVersion = meta.minorVersion; // != 0 ? meta.minorVersion : m_minorVersion;
    // m_majorVersion = majorVersion;
    // m_minorVersion = minorVersion;

    // writeLe32(packet.data() + 0, kFtasSyncReq);
    // writeLe16(packet.data() + 4, static_cast<std::uint16_t>(kPacketHeaderBytes));
    // writeLe16(packet.data() + 6, minorVersion);
    // writeLe16(packet.data() + 8, majorVersion);
    // writeLe32(packet.data() + 10, m_sequence);
    // writeLe16(packet.data() + 14, 0);

    // writeLe16(packet.data() + 16, meta.bodyType);
    // writeLe32(packet.data() + 18, static_cast<std::uint32_t>(kIqFixedBodyBytes + iqBytes));
    // writeLe32(packet.data() + 22, meta.sampleRate);
    // writeLe64(packet.data() + 26, meta.frequencyHz);
    // writeLe32(packet.data() + 34, meta.bandwidth);
    // writeLe64(packet.data() + 38, m_sampleCount);

    // std::memcpy(packet.data() + 46, samples, iqBytes);

    // const ssize_t sent = ::send(m_socketFd, packet.data(), packet.size(), 0);
    // if (sent < 0)
    // {
    //     throw std::system_error(errno, std::generic_category(), "Failed to send UDP IQ packet");
    // }
    // if (static_cast<std::size_t>(sent) != packet.size())
    // {
    //     throw std::runtime_error("Short UDP send");
    // }

    ++m_sequence;
    m_sampleCount += iqPairs;
}

std::optional<UdpIqClient::ResponsePacket> UdpIqClient::receivePacket(std::chrono::milliseconds timeout)
{
    // if (!isOpen())
    // {
    //     return std::nullopt;
    // }

    const size_t FTASD_SPLIT_PARSER_UDP_BUF_CAP = 65535;
    uint8_t raw_buf[FTASD_SPLIT_PARSER_UDP_BUF_CAP] = {0};
    size_t raw_len = 0u;
    ftas_udp_packet_t packet;
    int rc;

    rc = ftas_udp_recv(&m_ftas_udp_client, raw_buf, sizeof(raw_buf), &raw_len);
    if (rc == FTAS_ERR_TIMEOUT)
    {
        return {};
    }
    if (rc != FTAS_OK)
    {
        // fprintf(stderr,
        //         "%s: parser UDP recv warning: %s\n",
        //         "ftasd-parser",
        //         ftas_strerror(rc));
        return {};
    }

    rc = ftas_udp_decode_packet(raw_buf, raw_len, &packet);
    if (rc != FTAS_OK)
    {
        return {};
    }

    auto is_digital_cheat_type = [](uint16_t signal_type) -> int
    {
        return signal_type == 23 || signal_type == 41 || signal_type == 42 ||
               signal_type == 46 || signal_type == 47 || signal_type == 51;
    };

    if(!is_digital_cheat_type(packet.result.signal_type))
    {
        return {};
    }

    if (packet.raw_body_len < sizeof(DigitalCheat))
    {
        std::cout << "" << std::endl;
        return {};
    }

    const DigitalCheat *digitalCheat = (DigitalCheat *)(packet.raw_body);
    std::u16string utf16((char16_t *)digitalCheat->info, digitalCheat->infoLength);
    std::string info = to_utf8(utf16);

    std::cout << "info : " << info << std::endl;

    // fd_set readSet;
    // FD_ZERO(&readSet);
    // FD_SET(m_socketRx, &readSet);

    // timeval tv{};
    // const auto timeoutUs = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    // tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeoutUs.count() / 1000000);
    // tv.tv_usec = static_cast<decltype(tv.tv_usec)>(timeoutUs.count() % 1000000);

    // const int ready = ::select(m_socketRx + 1, &readSet, nullptr, nullptr, &tv);
    // if (ready < 0)
    // {
    //     throw std::system_error(errno, std::generic_category(), "UDP select failed");
    // }
    // if (ready == 0 || !FD_ISSET(m_socketRx, &readSet))
    // {
    //     return std::nullopt;
    // }

    // std::array<std::uint8_t, kMaxPacketBytes> recvBuffer{};
    // const ssize_t received = ::recv(m_socketRx, recvBuffer.data(), recvBuffer.size(), 0);
    // if (received < 0)
    // {
    //     if (errno == EAGAIN || errno == EWOULDBLOCK)
    //     {
    //         return std::nullopt;
    //     }
    //     throw std::system_error(errno, std::generic_category(), "UDP receive failed");
    // }
    // if (received == 0)
    // {
    //     return std::nullopt;
    // }

    // return decodePacket(std::vector<std::uint8_t>(recvBuffer.begin(), recvBuffer.begin() + received));
    return {};
}
