#include "UdpIqClient.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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
}

UdpIqClient::UdpIqClient() = default;

UdpIqClient::~UdpIqClient()
{
    close();
}

void UdpIqClient::open(const Endpoint &remote, const BindEndpoint &local)
{
    close();

    m_socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socketFd < 0)
    {
        throw std::system_error(errno, std::generic_category(), "Failed to create UDP socket");
    }

    m_socketRx = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socketRx < 0)
    {
        throw std::system_error(errno, std::generic_category(), "Failed to create receiver UDP socket");
    }

    try
    {
        sockaddr_in localAddr = makeSockaddr(local.address, local.port);
        if (::bind(m_socketFd, reinterpret_cast<const sockaddr *>(&localAddr), sizeof(localAddr)) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "Failed to bind UDP socket");
        }

        const sockaddr_in remoteAddr = makeSockaddr(remote.address, remote.port);
        if (::connect(m_socketFd, reinterpret_cast<const sockaddr *>(&remoteAddr), sizeof(remoteAddr)) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "Failed to connect UDP socket");
        }

        localAddr = makeSockaddr(local.address, 10111);
        if (::bind(m_socketRx, reinterpret_cast<const sockaddr *>(&localAddr), sizeof(localAddr)) != 0)
        {
            throw std::system_error(errno, std::generic_category(), "Failed to bind recever UDP socket");
        }

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

void UdpIqClient::sendInterleaved(const SendMeta &meta,
                                  const std::complex<std::int16_t> *samples,
                                  std::uint32_t iqPairs)
{
    if (!isOpen())
    {
        throw std::runtime_error("UDP socket is not open");
    }
    if (samples == nullptr || iqPairs == 0)
    {
        throw std::runtime_error("No IQ samples provided for UDP send");
    }

    const std::size_t iqBytes = static_cast<std::size_t>(iqPairs) * 4U;
    std::vector<std::uint8_t> packet(kPacketHeaderBytes + kIqFixedBodyBytes + iqBytes);

    const std::uint16_t majorVersion = meta.majorVersion;// != 0 ? meta.majorVersion : m_majorVersion;
    const std::uint16_t minorVersion = meta.minorVersion;// != 0 ? meta.minorVersion : m_minorVersion;
    m_majorVersion = majorVersion;
    m_minorVersion = minorVersion;

    writeLe32(packet.data() + 0, kFtasSyncReq);
    writeLe16(packet.data() + 4, static_cast<std::uint16_t>(kPacketHeaderBytes));
    writeLe16(packet.data() + 6, minorVersion);
    writeLe16(packet.data() + 8, majorVersion);
    writeLe32(packet.data() + 10, m_sequence);
    writeLe16(packet.data() + 14, 0);

    writeLe16(packet.data() + 16, meta.bodyType);
    writeLe32(packet.data() + 18, static_cast<std::uint32_t>(kIqFixedBodyBytes + iqBytes));
    writeLe32(packet.data() + 22, meta.sampleRate);
    writeLe64(packet.data() + 26, meta.frequencyHz);
    writeLe32(packet.data() + 34, meta.bandwidth);
    writeLe64(packet.data() + 38, m_sampleCount);

    std::memcpy(packet.data() + 46, samples, iqBytes);

    const ssize_t sent = ::send(m_socketFd, packet.data(), packet.size(), 0);
    if (sent < 0)
    {
        throw std::system_error(errno, std::generic_category(), "Failed to send UDP IQ packet");
    }
    if (static_cast<std::size_t>(sent) != packet.size())
    {
        throw std::runtime_error("Short UDP send");
    }

    ++m_sequence;
    m_sampleCount += iqPairs;
}

std::optional<UdpIqClient::ResponsePacket> UdpIqClient::receivePacket(std::chrono::milliseconds timeout)
{
    if (!isOpen())
    {
        return std::nullopt;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_socketRx, &readSet);

    timeval tv{};
    const auto timeoutUs = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeoutUs.count() / 1000000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>(timeoutUs.count() % 1000000);

    const int ready = ::select(m_socketRx + 1, &readSet, nullptr, nullptr, &tv);
    if (ready < 0)
    {
        throw std::system_error(errno, std::generic_category(), "UDP select failed");
    }
    if (ready == 0 || !FD_ISSET(m_socketRx, &readSet))
    {
        return std::nullopt;
    }

    std::array<std::uint8_t, kMaxPacketBytes> recvBuffer{};
    const ssize_t received = ::recv(m_socketRx, recvBuffer.data(), recvBuffer.size(), 0);
    if (received < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return std::nullopt;
        }
        throw std::system_error(errno, std::generic_category(), "UDP receive failed");
    }
    if (received == 0)
    {
        return std::nullopt;
    }

    return decodePacket(std::vector<std::uint8_t>(recvBuffer.begin(), recvBuffer.begin() + received));
}
