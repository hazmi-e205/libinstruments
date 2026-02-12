#include "../../include/instruments/performance_service.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"
#include <cstdlib>

namespace instruments {

static const char* TAG = "PerfService";

PerformanceService::PerformanceService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

PerformanceService::~PerformanceService() {
    Stop();
}

Error PerformanceService::GetAttributes(const std::string& selector,
                                         std::vector<std::string>& outAttrs) {
    auto dtxConn = m_connection->CreateInstrumentConnection();
    if (!dtxConn) return Error::ConnectionFailed;

    auto channel = dtxConn->MakeChannelWithIdentifier(ChannelId::DeviceInfo);
    if (!channel) return Error::ServiceStartFailed;

    auto msg = DTXMessage::CreateWithSelector(selector);
    auto response = channel->SendMessageSync(msg);
    channel->Cancel();

    if (!response) return Error::Timeout;

    auto payload = response->PayloadObject();
    if (!payload || !payload->IsArray()) {
        INST_LOG_WARN(TAG, "Unexpected attributes format for %s", selector.c_str());
        return Error::ProtocolError;
    }

    for (const auto& item : payload->AsArray()) {
        if (item.IsString()) {
            outAttrs.push_back(item.AsString());
        }
    }

    INST_LOG_DEBUG(TAG, "Got %zu attributes for %s", outAttrs.size(), selector.c_str());
    return Error::Success;
}

Error PerformanceService::GetSystemAttributes(std::vector<std::string>& outAttrs) {
    return GetAttributes("sysmonSystemAttributes", outAttrs);
}

Error PerformanceService::GetProcessAttributes(std::vector<std::string>& outAttrs) {
    return GetAttributes("sysmonProcessAttributes", outAttrs);
}

Error PerformanceService::Start(const PerfConfig& config,
                                 SystemPerfCallback systemCb,
                                 ProcessPerfCallback processCb,
                                 ErrorCallback errorCb) {
    if (m_running.load()) {
        Stop();
    }

    // Create persistent DTX connection
    m_dtxConnection = m_connection->CreateInstrumentConnection();
    if (!m_dtxConnection) {
        if (errorCb) errorCb(Error::ConnectionFailed, "Failed to create instrument connection");
        return Error::ConnectionFailed;
    }

    // Auto-populate attributes if empty
    PerfConfig actualConfig = config;
    if (actualConfig.systemAttributes.empty()) {
        GetSystemAttributes(actualConfig.systemAttributes);
        if (actualConfig.systemAttributes.empty()) {
            // Default attributes
            actualConfig.systemAttributes = {
                "cpu_total_load", "cpu_user_load", "cpu_system_load",
                "physMemSize", "memUsed", "vmExtPageCount",
                "diskBytesRead", "diskBytesWritten",
                "netBytesIn", "netBytesOut", "netPacketsIn", "netPacketsOut"
            };
        }
    }
    if (actualConfig.processAttributes.empty()) {
        GetProcessAttributes(actualConfig.processAttributes);
        if (actualConfig.processAttributes.empty()) {
            actualConfig.processAttributes = {
                "pid", "name", "cpuUsage", "physFootprint",
                "memAnon", "memVirtualSize",
                "diskBytesRead", "diskBytesWritten", "threadCount"
            };
        }
    }

    // Create sysmontap channel
    m_channel = m_dtxConnection->MakeChannelWithIdentifier(ChannelId::Sysmontap);
    if (!m_channel) {
        if (errorCb) errorCb(Error::ServiceStartFailed, "Failed to create sysmontap channel");
        return Error::ServiceStartFailed;
    }

    // Build config dictionary
    NSObject::DictType configDict;
    configDict["ur"] = NSObject(static_cast<int64_t>(actualConfig.sampleIntervalMs));
    configDict["bm"] = NSObject(static_cast<int32_t>(0));
    configDict["cpuUsage"] = NSObject(true);
    configDict["physFootprint"] = NSObject(true);
    configDict["sampleInterval"] = NSObject(static_cast<int64_t>(
        actualConfig.sampleIntervalMs * 1000000LL));

    // Process attributes as NSSet
    NSObject::ArrayType procAttrs;
    for (const auto& attr : actualConfig.processAttributes) {
        procAttrs.push_back(NSObject(attr));
    }
    NSObject procSet = NSObject::Set(std::move(procAttrs));
    procSet.SetClassName("NSSet");
    procSet.SetClassHierarchy({"NSSet", "NSObject"});
    configDict["procAttrs"] = procSet;

    // System attributes as NSSet
    NSObject::ArrayType sysAttrs;
    for (const auto& attr : actualConfig.systemAttributes) {
        sysAttrs.push_back(NSObject(attr));
    }
    NSObject sysSet = NSObject::Set(std::move(sysAttrs));
    sysSet.SetClassName("NSSet");
    sysSet.SetClassHierarchy({"NSSet", "NSObject"});
    configDict["sysAttrs"] = sysSet;

    NSObject configObj(std::move(configDict));
    configObj.SetClassName("NSMutableDictionary");
    configObj.SetClassHierarchy({"NSMutableDictionary", "NSDictionary", "NSObject"});

    // Store process attribute order for array-format parsing
    m_processAttributes = actualConfig.processAttributes;

    // Send setConfig:
    auto setConfigMsg = DTXMessage::CreateWithSelector("setConfig:");
    setConfigMsg->AppendAuxiliary(configObj);
    auto response = m_channel->SendMessageSync(setConfigMsg);
    if (!response) {
        if (errorCb) errorCb(Error::Timeout, "setConfig timeout");
        return Error::Timeout;
    }

    // Set message handler for streaming data
    m_channel->SetMessageHandler([this, systemCb, processCb, errorCb](
        std::shared_ptr<DTXMessage> msg) {
        if (!m_running.load()) return;

        auto payload = msg->PayloadObject();
        if (payload) {
            ParseSysmontapMessage(*payload, systemCb, processCb);
        }
    });

    // Send start
    auto startMsg = DTXMessage::CreateWithSelector("start");
    response = m_channel->SendMessageSync(startMsg);

    m_running.store(true);
    INST_LOG_INFO(TAG, "Performance monitoring started (interval=%ums)",
                 actualConfig.sampleIntervalMs);

    return Error::Success;
}

void PerformanceService::Stop() {
    if (!m_running.exchange(false)) return;

    INST_LOG_INFO(TAG, "Stopping performance monitoring");

    if (m_channel) {
        auto stopMsg = DTXMessage::CreateWithSelector("stop");
        m_channel->SendMessageSync(stopMsg, 2000);
        m_channel->Cancel();
        m_channel.reset();
    }

    if (m_dtxConnection) {
        m_dtxConnection->Disconnect();
        m_dtxConnection.reset();
    }
}

void PerformanceService::ParseSysmontapMessage(const NSObject& data,
                                                 SystemPerfCallback systemCb,
                                                 ProcessPerfCallback processCb) {
    if (!data.IsDict()) return;

    // System metrics
    if (systemCb && data.HasKey("SystemCPUUsage")) {
        SystemMetrics metrics;
        const auto& cpuUsage = data["SystemCPUUsage"];
        if (cpuUsage.IsDict()) {
            if (cpuUsage.HasKey("CPU_TotalLoad"))
                metrics.cpuTotalLoad = cpuUsage["CPU_TotalLoad"].ToNumber();
            if (cpuUsage.HasKey("CPU_UserLoad"))
                metrics.cpuUserLoad = cpuUsage["CPU_UserLoad"].ToNumber();
            if (cpuUsage.HasKey("CPU_SystemLoad"))
                metrics.cpuSystemLoad = cpuUsage["CPU_SystemLoad"].ToNumber();
        }
        if (data.HasKey("CPUCount"))
            metrics.cpuCount = static_cast<uint64_t>(data["CPUCount"].ToNumber());
        if (data.HasKey("EnabledCPUs"))
            metrics.enabledCPUs = static_cast<uint64_t>(data["EnabledCPUs"].ToNumber());

        systemCb(metrics);
    }

    // Process metrics
    if (processCb && data.HasKey("Processes")) {
        const auto& procs = data["Processes"];
        if (procs.IsDict()) {
            std::vector<ProcessMetrics> processMetrics;
            for (const auto& [pidStr, procData] : procs.AsDict()) {
                ProcessMetrics pm;
                pm.pid = std::atoll(pidStr.c_str());

                if (procData.IsArray() && !procData.AsArray().empty()) {
                    // Array format: values in same order as configured processAttributes
                    const auto& values = procData.AsArray();
                    for (size_t i = 0; i < values.size() && i < m_processAttributes.size(); i++) {
                        const auto& attr = m_processAttributes[i];
                        const auto& val = values[i];
                        if (attr == "pid")
                            pm.pid = static_cast<int64_t>(val.ToNumber());
                        else if (attr == "name" && val.IsString())
                            pm.name = val.AsString();
                        else if (attr == "cpuUsage")
                            pm.cpuUsage = val.ToNumber();
                        else if (attr == "physFootprint")
                            pm.memResident = static_cast<uint64_t>(val.ToNumber());
                        else if (attr == "memAnon")
                            pm.memAnon = static_cast<uint64_t>(val.ToNumber());
                        else if (attr == "memVirtualSize")
                            pm.memVirtual = static_cast<uint64_t>(val.ToNumber());
                        else if (attr == "diskBytesRead")
                            pm.diskBytesRead = static_cast<uint64_t>(val.ToNumber());
                        else if (attr == "diskBytesWritten")
                            pm.diskBytesWritten = static_cast<uint64_t>(val.ToNumber());
                        else if (attr == "threadCount")
                            pm.threads = static_cast<uint64_t>(val.ToNumber());
                    }
                } else if (procData.IsDict()) {
                    // Dict format (some iOS versions return dict)
                    if (procData.HasKey("pid"))
                        pm.pid = static_cast<int64_t>(procData["pid"].ToNumber());
                    if (procData.HasKey("name") && procData["name"].IsString())
                        pm.name = procData["name"].AsString();
                    if (procData.HasKey("cpuUsage"))
                        pm.cpuUsage = procData["cpuUsage"].ToNumber();
                    if (procData.HasKey("physFootprint"))
                        pm.memResident = static_cast<uint64_t>(procData["physFootprint"].ToNumber());
                    if (procData.HasKey("memAnon"))
                        pm.memAnon = static_cast<uint64_t>(procData["memAnon"].ToNumber());
                    if (procData.HasKey("memVirtualSize"))
                        pm.memVirtual = static_cast<uint64_t>(procData["memVirtualSize"].ToNumber());
                    if (procData.HasKey("diskBytesRead"))
                        pm.diskBytesRead = static_cast<uint64_t>(procData["diskBytesRead"].ToNumber());
                    if (procData.HasKey("diskBytesWritten"))
                        pm.diskBytesWritten = static_cast<uint64_t>(procData["diskBytesWritten"].ToNumber());
                    if (procData.HasKey("threadCount"))
                        pm.threads = static_cast<uint64_t>(procData["threadCount"].ToNumber());
                } else {
                    continue;
                }

                processMetrics.push_back(std::move(pm));
            }

            if (!processMetrics.empty()) {
                processCb(processMetrics);
            }
        }
    }
}

} // namespace instruments
