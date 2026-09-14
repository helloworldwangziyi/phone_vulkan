/**
 * @file platform_channel_test.cpp
 * @brief 平台通道（PlatformChannel）的纯 core 测试。
 *
 * 覆盖：handler 注册与按方法名路由、参数透传、未注册方法返回空串、
 * removePlatformHandler 后落空、重复注册覆盖、出向未注入 invoker 时
 * 返回 false、注入后往返且 resultOut 可空。
 *
 * 与 ui_runtime_test 同式：assert 断言 + printf 进度。
 */

#include <cassert>
#include <cstdio>
#include <string>

#include "evk/platform_channel.h"

namespace {

/// 每个用例开始前清场：摘除测试用过的方法名并注销出向实现。
void resetChannel() {
    evk::removePlatformHandler("echo");
    evk::removePlatformHandler("deviceInfo");
    evk::removePlatformHandler("missing");
    evk::setPlatformInvoker(nullptr);
}

/// 注册后按名路由命中，参数原样透传。
void testRegisterAndRoute() {
    resetChannel();
    std::string seenArgs;
    evk::setPlatformHandler("echo", [&seenArgs](const std::string& args) {
        seenArgs = args;
        return "echo:" + args;
    });
    evk::setPlatformHandler("deviceInfo",
                            [](const std::string&) { return "test-device"; });

    assert(evk::dispatchPlatformCall("echo", "hello") == "echo:hello");
    assert(seenArgs == "hello");                       // 参数透传
    assert(evk::dispatchPlatformCall("deviceInfo", "") == "test-device");
    // 未注册的方法不影响已注册方法的命中。
    assert(evk::dispatchPlatformCall("echo", "again") == "echo:again");
}

/// 未注册方法返回空串；nullptr 参数按空串处理。
void testUnregisteredReturnsEmpty() {
    resetChannel();
    assert(evk::dispatchPlatformCall("missing", "x").empty());
    evk::setPlatformHandler("echo", [](const std::string& args) {
        return "[" + args + "]";
    });
    assert(evk::dispatchPlatformCall("echo", nullptr) == "[]");
}

/// 摘除后路由落空；重复注册同名方法时新 handler 覆盖旧的。
void testRemoveAndRebind() {
    resetChannel();
    evk::setPlatformHandler("echo", [](const std::string&) { return "v1"; });
    assert(evk::dispatchPlatformCall("echo", "") == "v1");

    evk::setPlatformHandler("echo", [](const std::string&) { return "v2"; });
    assert(evk::dispatchPlatformCall("echo", "") == "v2");

    evk::removePlatformHandler("echo");
    assert(evk::dispatchPlatformCall("echo", "").empty());
    // 摘除未注册过的方法是无害空操作。
    evk::removePlatformHandler("echo");
}

/// 未注入 invoker 时 invokePlatform 返回 false，且不触碰 resultOut。
void testOutboundWithoutInvoker() {
    resetChannel();
    std::string out = "untouched";
    assert(!evk::invokePlatform("deviceInfo", "", &out));
    assert(out == "untouched");
    assert(!evk::invokePlatform("deviceInfo", "", nullptr));
}

/// 注入后往返：方法名/参数透传到平台实现，结果串写回 resultOut；
/// resultOut 传空同样返回 true（不关心返回值的调用方）。
void testOutboundRoundTrip() {
    resetChannel();
    std::string seenMethod;
    std::string seenArgs;
    evk::setPlatformInvoker(
        [&seenMethod, &seenArgs](const std::string& method, const std::string& args) {
            seenMethod = method;
            seenArgs = args;
            return "android 14";
        });

    std::string out;
    assert(evk::invokePlatform("deviceInfo", "{}", &out));
    assert(out == "android 14");
    assert(seenMethod == "deviceInfo");
    assert(seenArgs == "{}");

    assert(evk::invokePlatform("deviceInfo", "", nullptr));

    // 注销后恢复未注入语义。
    evk::setPlatformInvoker(nullptr);
    assert(!evk::invokePlatform("deviceInfo", "", nullptr));
}

} // namespace

int main() {
    testRegisterAndRoute();
    std::printf("ok: register and route\n");
    testUnregisteredReturnsEmpty();
    std::printf("ok: unregistered returns empty\n");
    testRemoveAndRebind();
    std::printf("ok: remove and rebind\n");
    testOutboundWithoutInvoker();
    std::printf("ok: outbound without invoker returns false\n");
    testOutboundRoundTrip();
    std::printf("ok: outbound round trip\n");
    std::printf("platform_channel_test: all passed\n");
    return 0;
}
