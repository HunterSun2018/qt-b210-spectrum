#pragma once

#include <chrono>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class UdpIqClient
{
public:
    struct Endpoint
    {
        std::string address = "127.0.0.1";
        std::uint16_t port = 9000;
    };

    struct BindEndpoint
    {
        std::string address = "0.0.0.0";
        std::uint16_t port = 0;
    };

    struct SendMeta
    {
        std::uint16_t majorVersion = 1;
        std::uint16_t minorVersion = 0;
        std::uint16_t bodyType = 202;
        std::uint32_t sampleRate = 0;
        std::uint64_t frequencyHz = 0;
        std::uint32_t bandwidth = 0;
    };

    struct ResponseHeader
    {
        std::uint32_t sync = 0;
        std::uint16_t length = 0;
        std::uint16_t minorVersion = 0;
        std::uint16_t majorVersion = 0;
        std::uint16_t headType = 0;
        std::int32_t power = 0;
    };

    struct ResultBody
    {
        std::uint16_t bodyType = 0;
        std::uint32_t bodyLength = 0;
        std::uint16_t signalType = 0;
        std::uint32_t signalFreq = 0;
        std::uint16_t signalBodyLength = 0;
        std::vector<std::uint8_t> signalBody;
    };

    struct JamIqBody
    {
        std::uint16_t bodyType = 0;
        std::uint32_t bodyLength = 0;
        std::uint32_t sampleRate = 0;
        std::uint32_t bandwidth = 0;
        std::uint64_t sampleCount = 0;
        std::vector<std::complex<std::int16_t>> iqSamples;
    };

    struct ResponsePacket
    {
        ResponseHeader header;
        ResultBody result;
        JamIqBody jamIq;
        std::vector<std::uint8_t> rawBody;
    };

    UdpIqClient();
    ~UdpIqClient();

    UdpIqClient(const UdpIqClient &) = delete;
    UdpIqClient &operator=(const UdpIqClient &) = delete;

    void open(const Endpoint &remote, const BindEndpoint &local);
    void close();

    bool isOpen() const;

    void sendInterleaved(const SendMeta &meta, const std::complex<std::int16_t> *samples, std::uint32_t iqPairs);
    std::optional<ResponsePacket> receivePacket(std::chrono::milliseconds timeout);

private:
    int m_socketFd = -1, m_socketRx = -1;
    std::uint16_t m_majorVersion = 1;
    std::uint16_t m_minorVersion = 0;   
    std::uint32_t m_sequence = 0;
    std::uint64_t m_sampleCount = 0;
};
