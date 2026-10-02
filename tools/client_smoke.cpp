// tools/client_smoke.cpp
// 测试客户端：连上 echo 服务器，发一个协议包（cmd=1, seq=42, payload=3 字节），
// 然后接收服务器 echo 回来的包，验证收发链路。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net/codec.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main() {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[client] WSAStartup 失败\n");
        return 1;
    }

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        std::printf("[client] socket 失败 err=%d\n", ::WSAGetLastError());
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(9527);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::printf("[client] connect 失败 err=%d\n", ::WSAGetLastError());
        return 1;
    }
    std::printf("[client] 已连接 127.0.0.1:9527\n");

    // 构造协议包：cmd=1, seq=42, payload = {0xDE, 0xAD, 0xBE}
    const std::uint8_t payload[3] = {0xDE, 0xAD, 0xBE};
    auto packet = arena::net::encode(1, 42, payload, sizeof(payload));

    // 发送
    int total = 0;
    while (total < static_cast<int>(packet.size())) {
        const int n = ::send(s, reinterpret_cast<const char*>(packet.data() + total),
                             static_cast<int>(packet.size() - total), 0);
        if (n <= 0) {
            std::printf("[client] send 失败 err=%d\n", ::WSAGetLastError());
            break;
        }
        total += n;
    }
    std::printf("[client] 已发送 %d 字节\n", total);

    // 接收 echo 回来的包
    std::uint8_t buf[128];
    const int got = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (got > 0) {
        std::printf("[client] 收到 %d 字节: ", got);
        for (int i = 0; i < got; ++i) {
            std::printf("%02X ", buf[i]);
        }
        std::printf("\n");

        // 尝试解包
        arena::net::Packet pkt;
        bool parsed = false;
        if (got >= 8) {
            const std::uint32_t len = arena::net::load_le32(buf);
            if (len == static_cast<std::uint32_t>(got - 4)) {
                pkt.cmd = arena::net::load_le16(buf + 4);
                pkt.seq = arena::net::load_le16(buf + 6);
                parsed = true;
            }
        }
        if (parsed) {
            std::printf("[client] 解包成功: cmd=%u seq=%u  ← 服务器 echo 回来了！\n", pkt.cmd, pkt.seq);
        } else {
            std::printf("[client] 解包失败（数据不完整或格式不对）\n");
        }
    } else {
        std::printf("[client] recv 失败/关闭 err=%d\n", ::WSAGetLastError());
    }

    ::closesocket(s);
    ::WSACleanup();
    return 0;
}
