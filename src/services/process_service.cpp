#include "../../include/instruments/process_service.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"

namespace instruments {

static const char* TAG = "ProcessService";

ProcessService::ProcessService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

ProcessService::~ProcessService() = default;

Error ProcessService::GetProcessList(std::vector<ProcessInfo>& outProcesses) {
    if (!m_connection) return Error::ConnectionFailed;

    if (m_connection->IsRSD()) {
        return GetProcessListXPC(outProcesses);
    }
    return GetProcessListDTX(outProcesses);
}

Error ProcessService::LaunchApp(const std::string& bundleId,
                                const std::map<std::string, std::string>& env,
                                const std::vector<std::string>& args,
                                bool killExisting,
                                int64_t& outPid) {
    if (!m_connection) return Error::ConnectionFailed;

    if (m_connection->IsRSD()) {
        return LaunchAppXPC(bundleId, env, args, killExisting, outPid);
    }
    return LaunchAppDTX(bundleId, env, args, killExisting, outPid);
}

Error ProcessService::LaunchApp(const std::string& bundleId, int64_t& outPid) {
    return LaunchApp(bundleId, {}, {}, true, outPid);
}

Error ProcessService::KillProcess(int64_t pid) {
    if (!m_connection) return Error::ConnectionFailed;

    if (m_connection->IsRSD()) {
        return KillProcessXPC(pid);
    }
    return KillProcessDTX(pid);
}

Error ProcessService::DisableMemoryLimit(int64_t pid) {
    auto dtxConn = m_connection->CreateInstrumentConnection();
    if (!dtxConn) return Error::ConnectionFailed;

    auto channel = dtxConn->MakeChannelWithIdentifier(ChannelId::ProcessControl);
    if (!channel) return Error::ServiceStartFailed;

    auto msg = DTXMessage::CreateWithSelector("requestDisableMemoryLimitsForPid:");
    msg->AppendAuxiliary(NSObject(pid));

    auto response = channel->SendMessageSync(msg);
    channel->Cancel();

    if (!response) return Error::Timeout;

    auto payload = response->PayloadObject();
    INST_LOG_DEBUG(TAG, "DisableMemoryLimit result: %s",
                  payload ? payload->ToJson().c_str() : "null");

    return Error::Success;
}

// --- DTX implementations ---

Error ProcessService::GetProcessListDTX(std::vector<ProcessInfo>& outProcesses) {
    auto dtxConn = m_connection->CreateInstrumentConnection();
    if (!dtxConn) return Error::ConnectionFailed;

    auto channel = dtxConn->MakeChannelWithIdentifier(ChannelId::DeviceInfo);
    if (!channel) return Error::ServiceStartFailed;

    auto msg = DTXMessage::CreateWithSelector("runningProcesses");
    auto response = channel->SendMessageSync(msg);
    channel->Cancel();

    if (!response) return Error::Timeout;

    auto payload = response->PayloadObject();
    if (!payload || !payload->IsArray()) {
        INST_LOG_ERROR(TAG, "Unexpected process list format");
        return Error::ProtocolError;
    }

    for (const auto& item : payload->AsArray()) {
        if (!item.IsDict()) continue;

        ProcessInfo proc;
        if (item.HasKey("pid"))
            proc.pid = static_cast<int64_t>(item["pid"].ToNumber());
        if (item.HasKey("name"))
            proc.name = item["name"].AsString();
        if (item.HasKey("bundleIdentifier"))
            proc.bundleId = item["bundleIdentifier"].AsString();
        if (item.HasKey("realAppName"))
            proc.realAppName = item["realAppName"].AsString();
        if (item.HasKey("isApplication"))
            proc.isApplication = item["isApplication"].AsBool();
        if (item.HasKey("startDate"))
            proc.startDate = item["startDate"].AsUInt64();

        outProcesses.push_back(std::move(proc));
    }

    INST_LOG_INFO(TAG, "Found %zu processes", outProcesses.size());
    return Error::Success;
}

Error ProcessService::LaunchAppDTX(const std::string& bundleId,
                                    const std::map<std::string, std::string>& env,
                                    const std::vector<std::string>& args,
                                    bool killExisting, int64_t& outPid) {
    auto dtxConn = m_connection->CreateInstrumentConnection();
    if (!dtxConn) return Error::ConnectionFailed;

    auto channel = dtxConn->MakeChannelWithIdentifier(ChannelId::ProcessControl);
    if (!channel) return Error::ServiceStartFailed;

    auto msg = DTXMessage::CreateWithSelector(
        "launchSuspendedProcessWithDevicePath:bundleIdentifier:environment:arguments:options:");

    // Arg 1: device path
    msg->AppendAuxiliary(NSObject(std::string("/private/")));

    // Arg 2: bundle identifier
    msg->AppendAuxiliary(NSObject(bundleId));

    // Arg 3: environment variables
    NSObject::DictType envDict;
    envDict["NSUnbufferedIO"] = NSObject(std::string("YES"));
    for (const auto& [key, val] : env) {
        envDict[key] = NSObject(val);
    }
    NSObject envObj(std::move(envDict));
    envObj.SetClassName("NSMutableDictionary");
    envObj.SetClassHierarchy({"NSMutableDictionary", "NSDictionary", "NSObject"});
    msg->AppendAuxiliary(envObj);

    // Arg 4: arguments
    NSObject::ArrayType argsArray;
    for (const auto& arg : args) {
        argsArray.push_back(NSObject(arg));
    }
    NSObject argsObj(std::move(argsArray));
    argsObj.SetClassName("NSMutableArray");
    argsObj.SetClassHierarchy({"NSMutableArray", "NSArray", "NSObject"});
    msg->AppendAuxiliary(argsObj);

    // Arg 5: options
    NSObject::DictType options;
    options["StartSuspendedKey"] = NSObject(static_cast<int64_t>(0));
    if (killExisting) {
        options["KillExisting"] = NSObject(static_cast<int64_t>(1));
    }
    options["ActivateSuspended"] = NSObject(static_cast<int64_t>(1));
    NSObject optionsObj(std::move(options));
    optionsObj.SetClassName("NSMutableDictionary");
    optionsObj.SetClassHierarchy({"NSMutableDictionary", "NSDictionary", "NSObject"});
    msg->AppendAuxiliary(optionsObj);

    auto response = channel->SendMessageSync(msg, 10000);
    channel->Cancel();

    if (!response) return Error::Timeout;

    auto payload = response->PayloadObject();
    if (payload) {
        outPid = static_cast<int64_t>(payload->ToNumber());
        INST_LOG_INFO(TAG, "Launched %s with PID %lld",
                     bundleId.c_str(), (long long)outPid);
    }

    return Error::Success;
}

Error ProcessService::KillProcessDTX(int64_t pid) {
    auto dtxConn = m_connection->CreateInstrumentConnection();
    if (!dtxConn) return Error::ConnectionFailed;

    auto channel = dtxConn->MakeChannelWithIdentifier(ChannelId::ProcessControl);
    if (!channel) return Error::ServiceStartFailed;

    auto msg = DTXMessage::CreateWithSelector("killPid:");
    msg->AppendAuxiliary(NSObject(static_cast<uint64_t>(pid)));

    auto response = channel->SendMessageSync(msg);
    channel->Cancel();

    INST_LOG_INFO(TAG, "Kill PID %lld: %s", (long long)pid,
                 response ? "success" : "timeout");

    return response ? Error::Success : Error::Timeout;
}

// --- XPC implementations (iOS 17+) ---

Error ProcessService::GetProcessListXPC(std::vector<ProcessInfo>& outProcesses) {
    // iOS 17+ uses CoreDevice AppService for process listing
    // Feature: com.apple.coredevice.feature.listprocesses

    // For now, fall back to DTX which also works on iOS 17+
    // when connected via tunnel
    return GetProcessListDTX(outProcesses);
}

Error ProcessService::LaunchAppXPC(const std::string& bundleId,
                                    const std::map<std::string, std::string>& env,
                                    const std::vector<std::string>& args,
                                    bool killExisting, int64_t& outPid) {
    // iOS 17+ uses CoreDevice AppService for app launch
    // Feature: com.apple.coredevice.feature.launchapplication

    // For now, fall back to DTX
    return LaunchAppDTX(bundleId, env, args, killExisting, outPid);
}

Error ProcessService::KillProcessXPC(int64_t pid) {
    // iOS 17+ uses CoreDevice AppService for killing
    // Feature: com.apple.coredevice.feature.sendsignaltoprocess

    // For now, fall back to DTX
    return KillProcessDTX(pid);
}

} // namespace instruments
