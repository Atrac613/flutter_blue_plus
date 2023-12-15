#include "include/flutter_blue_plus/flutter_blue_plus_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <flutter/method_channel.h>
#include <flutter/basic_message_channel.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/standard_message_codec.h>

#include <map>
#include <memory>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

#define GUID_FORMAT "%08x-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx"
#define GUID_ARG(guid) guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]

namespace {

    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::Foundation::Collections;
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::Devices::Radios;
    using namespace winrt::Windows::Devices::Bluetooth;
    using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

    using flutter::EncodableValue;
    using flutter::EncodableMap;
    using flutter::EncodableList;

    union uint16_t_union {
        uint16_t uint16;
        byte bytes[sizeof(uint16_t)];
    };

    std::vector<uint8_t> to_bytevc(IBuffer buffer) {
        auto reader = DataReader::FromBuffer(buffer);
        auto result = std::vector<uint8_t>(reader.UnconsumedBufferLength());
        reader.ReadBytes(result);
        return result;
    }

    IBuffer from_bytevc(std::vector<uint8_t> bytes) {
        auto writer = DataWriter();
        writer.WriteBytes(bytes);
        return writer.DetachBuffer();
    }

    std::string to_hexstring(std::vector<uint8_t> bytes) {
        auto ss = std::stringstream();
        for (auto b : bytes)
            ss << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(b);
        return ss.str();
    }

    std::vector<uint8_t> hex_to_bytes(std::string hex) {
        std::vector<uint8_t> bytes;
        for (unsigned int i = 0; i < hex.length(); i += 2) {
            std::string bytestring = hex.substr(i, 2);
            uint8_t byte = (uint8_t) strtol(bytestring.c_str(), nullptr, 16);
            bytes.push_back(byte);
        }
        return bytes;
    }

    std::string to_uuidstr(winrt::guid guid) {
        char chars[36 + 1];
        sprintf_s(chars, GUID_FORMAT, GUID_ARG(guid));
        return std::string{ chars };
    }

    std::string remove_string(std::string text, std::string targetText) {
        text.erase(remove_if(text.begin(), text.end(),
                                 [&targetText](const char &c) {
                                     return targetText.find(c) != std::string::npos;
                                 }),
                   text.end());

        return text;
    }

    int to_bmAdapterState(RadioState state) {
        switch (state) {
            case RadioState::Disabled:
                return 6;
            case RadioState::Off:
                return 6;
            case RadioState::On:
                return 4;
            default:
                return 0;
        }
    }

    bool isAdapterOn(Radio radio) {
        auto state = radio ? radio.State() : RadioState::Unknown;
        return state == RadioState::On;
    }

    std::string getGattCommunicationStatusMessage(GattCommunicationStatus status) {
        switch (status) {
            case GattCommunicationStatus::AccessDenied:
                return "Access is denied.";
            case GattCommunicationStatus::ProtocolError:
                return "There was a GATT communication protocol error.";
            case GattCommunicationStatus::Unreachable:
                return "No communication can be performed with the device, at this time.";
            case GattCommunicationStatus::Success:
                return "The operation completed successfully.";
            default:
                return "Unknown status.";
        }
    }

    std::wstring formatBluetoothAddress(unsigned long long BluetoothAddress)
    {
        std::wostringstream ret;
        ret << std::hex << std::setfill(L'0')
            << std::setw(2) << ((BluetoothAddress >> (5 * 8)) & 0xff) << ":"
            << std::setw(2) << ((BluetoothAddress >> (4 * 8)) & 0xff) << ":"
            << std::setw(2) << ((BluetoothAddress >> (3 * 8)) & 0xff) << ":"
            << std::setw(2) << ((BluetoothAddress >> (2 * 8)) & 0xff) << ":"
            << std::setw(2) << ((BluetoothAddress >> (1 * 8)) & 0xff) << ":"
            << std::setw(2) << ((BluetoothAddress >> (0 * 8)) & 0xff);
        return ret.str();
    }

    enum LogLevel {
        LNONE = 0,
        LERROR = 1,
        LWARNING = 2,
        LINFO = 3,
        LDEBUG = 4,
        LVERBOSE = 5
    };

    struct BluetoothDeviceAgent {
        BluetoothLEDevice device;
        winrt::event_token connnectionStatusChangedToken;
        std::map<std::string, GattDeviceService> gattServices;
        std::map<std::string, GattCharacteristic> gattCharacteristics;
        std::map<std::string, winrt::event_token> valueChangedTokens;

        BluetoothDeviceAgent(BluetoothLEDevice device, winrt::event_token connnectionStatusChangedToken)
            : device(device),
            connnectionStatusChangedToken(connnectionStatusChangedToken) {}

        ~BluetoothDeviceAgent() {
            device = nullptr;
        }

        IAsyncOperation<GattDeviceService> GetServiceAsync(std::string service) {
            if (gattServices.count(service) == 0) {
                auto serviceResult = co_await device.GetGattServicesAsync();
                if (serviceResult.Status() != GattCommunicationStatus::Success)
                    co_return nullptr;

                for (auto s : serviceResult.Services()) {
                    std::string uuid = to_uuidstr(s.Uuid());
                    if (service.size() == 4) {
                        uuid = uuid.substr(4, 4);
                    }

                    if (uuid == service) {
                        gattServices.insert(std::make_pair(service, s));
                    }
                }
            }
            co_return gattServices.at(service);
        }

        IAsyncOperation<GattCharacteristic> GetCharacteristicAsync(std::string service, std::string characteristic) {
            if (gattCharacteristics.count(characteristic) == 0) {
                auto gattService = co_await GetServiceAsync(service);

                auto characteristicResult = co_await gattService.GetCharacteristicsAsync();
                if (characteristicResult.Status() != GattCommunicationStatus::Success)
                    co_return nullptr;

                for (auto c : characteristicResult.Characteristics()) {
                    std::string uuid = to_uuidstr(c.Uuid());
                    if (characteristic.size() == 4) {
                        uuid = uuid.substr(4, 4);
                    }

                    if (uuid == characteristic) {
                        gattCharacteristics.insert(std::make_pair(characteristic, c));
                    }
                }
            }
            co_return gattCharacteristics.at(characteristic);
        }
    };

    class FlutterBluePlusPlugin : public flutter::Plugin {
    public:
        static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

        FlutterBluePlusPlugin();

        virtual ~FlutterBluePlusPlugin();
    private:
        winrt::fire_and_forget InitializeAsync();

        // Called when a method is called on this plugin's channel from Dart.
        void HandleMethodCall(
            const flutter::MethodCall<flutter::EncodableValue>& method_call,
            std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

        std::unique_ptr<flutter::MethodChannel<EncodableValue>> method_channel_;

        EncodableList targetServiceUuids;

        Radio bluetoothRadio{ nullptr };

        BluetoothLEAdvertisementWatcher bluetoothLEWatcher{ nullptr };
        winrt::event_token bluetoothLEWatcherReceivedToken;
        void BluetoothLEWatcher_Received(BluetoothLEAdvertisementWatcher sender, BluetoothLEAdvertisementReceivedEventArgs args);
        winrt::fire_and_forget SendScanResultAsync(BluetoothLEAdvertisementReceivedEventArgs args);

        std::map<uint64_t, std::unique_ptr<BluetoothDeviceAgent>> connectedDevices{};

        winrt::fire_and_forget ConnectAsync(uint64_t bluetoothAddress);
        void BluetoothLEDevice_ConnectionStatusChanged(BluetoothLEDevice sender, IInspectable args);
        void CleanConnection(uint64_t bluetoothAddress);
        winrt::fire_and_forget DiscoverServicesAsync(BluetoothDeviceAgent& bluetoothDeviceAgent);
        winrt::fire_and_forget SetNotifiableAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic, int32_t bleInputProperty);
        winrt::fire_and_forget ReadValueAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic);
        winrt::fire_and_forget WriteValueAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic, std::vector<uint8_t> value, int32_t bleOutputProperty);
        void FlutterBluePlusPlugin::GattCharacteristic_ValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args);

        int32_t logLevel;
        void FlutterBluePlusPlugin::FBPLog(LogLevel level, winrt::hstring message);
    };

    // static
    void FlutterBluePlusPlugin::RegisterWithRegistrar(
        flutter::PluginRegistrarWindows* registrar) {
        auto method_channel_ =
            std::make_unique<flutter::MethodChannel<EncodableValue>>(
                registrar->messenger(), "flutter_blue_plus/methods",
                &flutter::StandardMethodCodec::GetInstance());

        auto plugin = std::make_unique<FlutterBluePlusPlugin>();

        method_channel_->SetMethodCallHandler(
            [plugin_pointer = plugin.get()](const auto& call, auto result) {
                plugin_pointer->HandleMethodCall(call, std::move(result));
            });

        plugin->method_channel_ = std::move(method_channel_);

        registrar->AddPlugin(std::move(plugin));
    }

    FlutterBluePlusPlugin::FlutterBluePlusPlugin() {
        InitializeAsync();
    }

    FlutterBluePlusPlugin::~FlutterBluePlusPlugin() {}

    winrt::fire_and_forget FlutterBluePlusPlugin::InitializeAsync() {
        auto bluetoothAdapter = co_await BluetoothAdapter::GetDefaultAsync();
        bluetoothRadio = co_await bluetoothAdapter.GetRadioAsync();
    }

    void FlutterBluePlusPlugin::HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        auto method_name = method_call.method_name();
        FBPLog(LDEBUG, L"MethodName: " + winrt::to_hstring(method_name));

        if (method_name.compare("flutterHotRestart") == 0) {
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("connectedCount") == 0) {
            result->Success(0);
        }
        else if (method_name.compare("setLogLevel") == 0) {
            logLevel = std::get<int32_t>(*method_call.arguments());
            FBPLog(LINFO, L"LogLevel: " + winrt::to_hstring(logLevel));

            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("getAdapterState") == 0) {
            result->Success(EncodableMap{
                {"adapter_state", to_bmAdapterState(bluetoothRadio ? bluetoothRadio.State() : RadioState::Unknown)}
            });
        }
        else if (method_name.compare("getSystemDevices") == 0) {
            EncodableList devices;
            result->Success(EncodableMap{
                    {"devices", devices}
            });
        }
        else if (method_name.compare("startScan") == 0) {
            const auto *arguments = std::get_if<EncodableMap>(method_call.arguments());

            // check adapter state
            if (isAdapterOn(bluetoothRadio) == false) {
                result->Error("startScan", "bluetooth must be turned on.");
                return;
            }

            auto withServices_it = arguments->find(EncodableValue("with_services"));
            if (withServices_it != arguments->end()) {
                targetServiceUuids = std::move(std::get<EncodableList>(withServices_it->second));
            }

            if (!bluetoothLEWatcher) {
                bluetoothLEWatcher = BluetoothLEAdvertisementWatcher();
                bluetoothLEWatcher.ScanningMode(BluetoothLEScanningMode::Active);
                bluetoothLEWatcherReceivedToken = bluetoothLEWatcher.Received({ this, &FlutterBluePlusPlugin::BluetoothLEWatcher_Received });
            }
            bluetoothLEWatcher.Start();
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("stopScan") == 0) {
            if (bluetoothLEWatcher) {
                bluetoothLEWatcher.Stop();
                bluetoothLEWatcher.Received(bluetoothLEWatcherReceivedToken);
            }
            bluetoothLEWatcher = nullptr;
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("connect") == 0) {
            auto args = std::get<EncodableMap>(*method_call.arguments());
            std::string remoteId = std::get<std::string>(args[EncodableValue("remote_id")]);
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            remoteId = remove_string(remoteId, ":");

            ConnectAsync(std::stoull(remoteId.c_str(), 0, 16));
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("disconnect") == 0) {
            std::string remoteId = std::get<std::string>(*method_call.arguments());
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            remoteId = remove_string(remoteId, ":");

            CleanConnection(std::stoull(remoteId.c_str(), 0, 16));
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("readRssi") == 0) {
            // No way currently to get connected RSSI on Windows
            // https://stackoverflow.com/questions/64096245/how-to-get-rssi-of-a-connected-bluetoothledevice-in-uwp

            std::string remoteId = std::get<std::string>(*method_call.arguments());
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            result->Success(EncodableValue(true));

            method_channel_->InvokeMethod("OnReadRssi",
                std::make_unique<EncodableValue>(EncodableMap{
                      {"remote_id", EncodableValue(remoteId)},
                      {"rssi", EncodableValue(0)},
                      {"success", EncodableValue(true)},
                      {"error_string", EncodableValue("success")},
                      {"error_code", EncodableValue(0)},
                }));
        }
        else if (method_name.compare("discoverServices") == 0) {
            std::string remoteId = std::get<std::string>(*method_call.arguments());
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            std::string remoteIdString = remove_string(remoteId, ":");
            auto remoteIdInt = std::stoull(remoteIdString.c_str(), 0, 16);

            auto it = connectedDevices.find(remoteIdInt);
            if (it == connectedDevices.end()) {
                result->Error("IllegalArgument", "Unknown remoteId:" + remoteId);
                return;
            }
            DiscoverServicesAsync(*it->second);
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("setNotifyValue") == 0) {
            auto args = std::get<EncodableMap>(*method_call.arguments());
            std::string remoteId = std::get<std::string>(args[EncodableValue("remote_id")]);
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            auto characteristicUuid = std::get<std::string>(args[EncodableValue("characteristic_uuid")]);
            auto serviceUuid = std::get<std::string>(args[EncodableValue("service_uuid")]);
            //auto secondaryServiceUuid = std::get<std::string>(args[EncodableValue("secondary_service_uuid")]);
            auto enable = std::get<bool>(args[EncodableValue("enable")]);

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            std::string remoteIdString = remove_string(remoteId, ":");
            auto remoteIdInt = std::stoull(remoteIdString.c_str(), 0, 16);

            auto it = connectedDevices.find(remoteIdInt);
            if (it == connectedDevices.end()) {
                result->Error("IllegalArgument", "Unknown remoteId:" + remoteId);
                return;
            }

            SetNotifiableAsync(*it->second, serviceUuid, characteristicUuid, enable ? 1 : 0);
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("requestMtu") == 0) {
            result->Error("requestMtu", "Windows does not allow mtu requests to the peripheral");
        }
        else if (method_name.compare("readCharacteristic") == 0) {
            auto args = std::get<EncodableMap>(*method_call.arguments());
            std::string remoteId = std::get<std::string>(args[EncodableValue("remote_id")]);
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            auto characteristicUuid = std::get<std::string>(args[EncodableValue("characteristic_uuid")]);
            auto serviceUuid = std::get<std::string>(args[EncodableValue("service_uuid")]);
            //auto secondaryServiceUuid = std::get<std::string>(args[EncodableValue("secondary_service_uuid")]);

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            std::string remoteIdString = remove_string(remoteId, ":");
            auto remoteIdInt = std::stoull(remoteIdString.c_str(), 0, 16);

            auto it = connectedDevices.find(remoteIdInt);
            if (it == connectedDevices.end()) {
                result->Error("IllegalArgument", "Unknown remoteId:" + remoteId);
                return;
            }

            ReadValueAsync(*it->second, serviceUuid, characteristicUuid);
            result->Success(EncodableValue(true));
        }
        else if (method_name.compare("writeCharacteristic") == 0) {
            auto args = std::get<EncodableMap>(*method_call.arguments());
            std::string remoteId = std::get<std::string>(args[EncodableValue("remote_id")]);
            FBPLog(LDEBUG, L"RemoteId: " + winrt::to_hstring(remoteId));

            auto characteristicUuid = std::get<std::string>(args[EncodableValue("characteristic_uuid")]);
            auto serviceUuid = std::get<std::string>(args[EncodableValue("service_uuid")]);
            //auto secondaryServiceUuid = std::get<std::string>(args[EncodableValue("secondary_service_uuid")]);
            auto writeType = std::get<int32_t>(args[EncodableValue("write_type")]);
            //auto allowLongWrite = std::get<int32_t>(args[EncodableValue("allow_long_write")]);
            auto value = std::get<std::string>(args[EncodableValue("value")]);

            auto hexValue = hex_to_bytes(value);

            // "d9:da:10:8a:32:3a" to "d9da108a323a"
            std::string remoteIdString = remove_string(remoteId, ":");
            auto remoteIdInt = std::stoull(remoteIdString.c_str(), 0, 16);

            auto it = connectedDevices.find(remoteIdInt);
            if (it == connectedDevices.end()) {
                result->Error("IllegalArgument", "Unknown remoteId:" + remoteId);
                return;
            }

            WriteValueAsync(*it->second, serviceUuid, characteristicUuid, hexValue, writeType);
            result->Success(EncodableValue(true));
        }
        else {
            result->NotImplemented();
        }
    }

    std::vector<uint8_t> parseManufacturerDataHead(BluetoothLEAdvertisement advertisement)
    {
        if (advertisement.ManufacturerData().Size() == 0)
            return std::vector<uint8_t>();

        auto manufacturerData = advertisement.ManufacturerData().GetAt(0);
        // FIXME Compat with REG_DWORD_BIG_ENDIAN
        uint8_t* prefix = uint16_t_union{ manufacturerData.CompanyId() }.bytes;
        auto result = std::vector<uint8_t>{ prefix, prefix + sizeof(uint16_t_union) };

        auto data = to_bytevc(manufacturerData.Data());
        result.insert(result.end(), data.begin(), data.end());
        return result;
    }

    void FlutterBluePlusPlugin::BluetoothLEWatcher_Received(
        BluetoothLEAdvertisementWatcher sender,
        BluetoothLEAdvertisementReceivedEventArgs args) {
        SendScanResultAsync(args);
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::SendScanResultAsync(BluetoothLEAdvertisementReceivedEventArgs args) {
        auto device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress());
        auto name = device ? device.Name() : args.Advertisement().LocalName();
        FBPLog(LDEBUG, L"Received BluetoothAddress:" + winrt::to_hstring(args.BluetoothAddress())
            + L", Name:" + name + L", LocalName:" + args.Advertisement().LocalName());

        bool hasService = false;
        if (targetServiceUuids.size() > 0) {
            IVector<winrt::guid> services = args.Advertisement().ServiceUuids();
            for (winrt::guid serviceUuid : services) {
                for (flutter::EncodableValue targetServiceUuid : targetServiceUuids) {
                    if (to_uuidstr(serviceUuid) == std::get<std::string>(targetServiceUuid)) {
                        hasService = true;
                        break;
                    }
                }
            }
        } else {
            hasService = true;
        }

        EncodableMap manufacturerData;
        for (auto const& data : args.Advertisement().ManufacturerData()) {
            auto manufacturerId = data.CompanyId();
            auto bytes = to_bytevc(data.Data());

            manufacturerData[EncodableValue(manufacturerId)] = EncodableValue(to_hexstring(bytes));
        }

        EncodableMap serviceData;
        for (auto const& data : args.Advertisement().GetSectionsByType(0x16)) {
            std::vector<uint8_t> bytes = to_bytevc(data.Data());

            std::vector<uint8_t> uuidBytes;
            std::vector<uint8_t> payloadBytes;

            auto dataSize = data.Data().Length();
            if (dataSize > 0) {
                size_t uuidSize = 0;

                if (dataSize >= 128) {
                    uuidSize = 16;
                } else if (dataSize >= 32) {
                    uuidSize = 4;
                } else {
                    uuidSize = 2;
                }

                uuidBytes.resize(uuidSize);
                for (size_t i=0; i<uuidSize; i++) {
                    uuidBytes[i] = bytes[i];
                }

                payloadBytes.resize(dataSize - uuidSize);
                for (size_t i=0; i<dataSize - uuidSize; i++) {
                    payloadBytes[i] = bytes[i + uuidSize];
                }
            }

            // Convert Little Endian to Big Endian
            std::reverse(uuidBytes.begin(), uuidBytes.end());
            std::reverse(payloadBytes.begin(), payloadBytes.end());

            serviceData[EncodableValue(to_hexstring(uuidBytes))] = EncodableValue(to_hexstring(payloadBytes));
        }

        EncodableValue txPower;
        if (args.TransmitPowerLevelInDBm()) {
            txPower = EncodableValue((short)args.TransmitPowerLevelInDBm().Value());
        }

        EncodableList serviceUuidList;
        IVector<winrt::guid> serviceUuids = args.Advertisement().ServiceUuids();
        for (winrt::guid uuid : serviceUuids) {
            serviceUuidList.push_back(EncodableValue(to_uuidstr(uuid)));
        }

        if (hasService) {
            EncodableList advertisements;
            advertisements.push_back(EncodableMap{
                {"remote_id", EncodableValue(winrt::to_string(formatBluetoothAddress(args.BluetoothAddress())))},
                {"platform_name", EncodableValue(winrt::to_string(name))},
                {"adv_name", EncodableValue(winrt::to_string(args.Advertisement().LocalName()))},
                {"connectable", EncodableValue(args.IsConnectable())},
                {"tx_power_level", txPower},
                {"manufacturer_data", EncodableValue(manufacturerData)},
                {"service_uuids", EncodableValue(serviceUuidList)},
                {"service_data", EncodableValue(serviceData)},
                {"rssi", EncodableValue(args.RawSignalStrengthInDBm())}
            });

            method_channel_->InvokeMethod("OnScanResponse", std::make_unique<EncodableValue>(EncodableMap{
                    {EncodableValue("advertisements"), advertisements},
            }));
        }
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::ConnectAsync(uint64_t bluetoothAddress) {
        auto device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(bluetoothAddress);
        auto servicesResult = co_await device.GetGattServicesAsync();
        if (servicesResult.Status() != GattCommunicationStatus::Success) {
            std::string errorMessage = getGattCommunicationStatusMessage(servicesResult.Status());
            FBPLog(LERROR, L"GetGattServicesAsync error: " + winrt::to_hstring(errorMessage));

            method_channel_->InvokeMethod("OnConnectionStateChanged",
                std::make_unique<EncodableValue>(EncodableMap{
                      {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
                      {"connection_state", EncodableValue(0)},
                      {"disconnect_reason_code", EncodableValue((int32_t)servicesResult.Status())},
                      {"disconnect_reason_string", EncodableValue(errorMessage)}
                }));

            co_return;
        }
        auto connnectionStatusChangedToken = device.ConnectionStatusChanged({ this, &FlutterBluePlusPlugin::BluetoothLEDevice_ConnectionStatusChanged });
        auto deviceAgent = std::make_unique<BluetoothDeviceAgent>(device, connnectionStatusChangedToken);
        auto pair = std::make_pair(bluetoothAddress, std::move(deviceAgent));
        connectedDevices.insert(std::move(pair));

        method_channel_->InvokeMethod("OnConnectionStateChanged",
            std::make_unique<EncodableValue>(EncodableMap{
              {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
              {"connection_state", EncodableValue(1)},
              {"disconnect_reason_code", EncodableValue()},
              {"disconnect_reason_string", EncodableValue()}
            }));
    }

    void FlutterBluePlusPlugin::BluetoothLEDevice_ConnectionStatusChanged(BluetoothLEDevice sender, IInspectable args) {
        FBPLog(LDEBUG, L"ConnectionStatusChanged " + winrt::to_hstring((int32_t)sender.ConnectionStatus()));
        if (sender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
            CleanConnection(sender.BluetoothAddress());

            method_channel_->InvokeMethod("OnConnectionStateChanged",
                std::make_unique<EncodableValue>(EncodableMap{
                      {"remote_id", winrt::to_string(formatBluetoothAddress(sender.BluetoothAddress()))},
                      {"connection_state", EncodableValue(0)},
                      {"disconnect_reason_code", EncodableValue()},
                      {"disconnect_reason_string", EncodableValue()}
                }));
        }
    }

    void FlutterBluePlusPlugin::CleanConnection(uint64_t bluetoothAddress) {
        auto node = connectedDevices.extract(bluetoothAddress);
        if (!node.empty()) {
            auto deviceAgent = std::move(node.mapped());
            deviceAgent->device.ConnectionStatusChanged(deviceAgent->connnectionStatusChangedToken);
            for (auto& tokenPair : deviceAgent->valueChangedTokens) {
                deviceAgent->gattCharacteristics.at(tokenPair.first).ValueChanged(tokenPair.second);
            }

            method_channel_->InvokeMethod("OnConnectionStateChanged",
                std::make_unique<EncodableValue>(EncodableMap{
                      {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
                      {"connection_state", EncodableValue(0)},
                      {"disconnect_reason_code", EncodableValue()},
                      {"disconnect_reason_string", EncodableValue()}
                }));
        }
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::DiscoverServicesAsync(BluetoothDeviceAgent& bluetoothDeviceAgent) {
        auto serviceResult = co_await bluetoothDeviceAgent.device.GetGattServicesAsync();
        if (serviceResult.Status() != GattCommunicationStatus::Success) {
            EncodableList services;
            method_channel_->InvokeMethod("OnDiscoveredServices",
                std::make_unique<EncodableValue>(EncodableMap{
                      {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothDeviceAgent.device.BluetoothAddress()))},
                      {"services", EncodableValue(services)},
                      {"success", EncodableValue(0)},
                      {"error_string", EncodableValue("Invalid status")},
                      {"error_code", EncodableValue(0)}
                }));
            co_return;
        }

        auto bluetoothAddress = bluetoothDeviceAgent.device.BluetoothAddress();
        EncodableList services;

        for (auto s : serviceResult.Services()) {
            EncodableList includedServices;
            auto includedServiceResult = co_await s.GetIncludedServicesAsync();
            if (includedServiceResult.Status() != GattCommunicationStatus::Success) {
                //includedServices = co_await bmBluetoothService(includedServiceResult, bluetoothAddress);
            }

            auto service = EncodableMap{
                    {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
                    {"service_uuid", to_uuidstr(s.Uuid())},
                    {"is_primary", EncodableValue(true)},
                    {"included_services", EncodableValue(includedServices)}
            };

            auto characteristicResult = co_await s.GetCharacteristicsAsync();
            if (characteristicResult.Status() == GattCommunicationStatus::Success) {
                EncodableList characteristics;
                for (auto c : characteristicResult.Characteristics()) {
                    auto descriptorsResult = co_await c.GetDescriptorsAsync();
                    EncodableList descriptors;
                    for (auto d : descriptorsResult.Descriptors()) {
                        descriptors.push_back(EncodableMap{
                                {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
                                {"service_uuid", EncodableValue(to_uuidstr(s.Uuid()))},
                                {"secondary_service_uuid", EncodableValue()},
                                {"characteristic_uuid", EncodableValue(to_uuidstr(c.Uuid()))},
                                {"descriptor_uuid", EncodableValue(to_uuidstr(d.Uuid()))},
                        });
                    }

                    auto props = (unsigned int)c.CharacteristicProperties();
                    auto propsMap = EncodableMap{
                            {"broadcast", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::Broadcast))},
                            {"read", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::Read))},
                            {"write_without_response", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::WriteWithoutResponse))},
                            {"write", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::Write))},
                            {"notify", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::Notify))},
                            {"indicate", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::Indicate))},
                            {"authenticated_signed_writes", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::AuthenticatedSignedWrites))},
                            {"extended_properties", EncodableValue((int32_t)(props & (unsigned int)GattCharacteristicProperties::ExtendedProperties))},
                            {"notify_encryption_required", EncodableValue(false)},
                            {"indicate_encryption_required", EncodableValue(false)}
                    };

                    characteristics.push_back(EncodableMap{
                            {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothAddress))},
                            {"service_uuid", to_uuidstr(c.Service().Uuid())},
                            {"secondary_service_uuid", EncodableValue()},
                            {"characteristic_uuid", to_uuidstr(c.Uuid())},
                            {"descriptors", EncodableValue(descriptors)},
                            {"properties", EncodableValue(propsMap)}
                    });
                }
                service.insert({ "characteristics", characteristics });
            }

            services.push_back(service);
        }

        method_channel_->InvokeMethod("OnDiscoveredServices",
        std::make_unique<EncodableValue>(EncodableMap{
              {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothDeviceAgent.device.BluetoothAddress()))},
              {"services", EncodableValue(services)},
              {"success", EncodableValue(1)},
              {"error_string", EncodableValue("success")},
              {"error_code", EncodableValue(0)}
        }));
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::SetNotifiableAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic, int32_t bleInputProperty) {
        FBPLog(LDEBUG, L"SetNotifiableAsync " + winrt::to_hstring((int32_t) bleInputProperty));

        auto gattCharacteristic = co_await bluetoothDeviceAgent.GetCharacteristicAsync(service, characteristic);
        auto descriptorValue = bleInputProperty == 1 ? GattClientCharacteristicConfigurationDescriptorValue::Notify
            : bleInputProperty == 2 ? GattClientCharacteristicConfigurationDescriptorValue::Indicate
            : GattClientCharacteristicConfigurationDescriptorValue::None;

        auto writeDescriptorStatus = co_await gattCharacteristic.WriteClientCharacteristicConfigurationDescriptorAsync(descriptorValue);
        FBPLog(LDEBUG, L"WriteClientCharacteristicConfigurationDescriptorAsync " + winrt::to_hstring((int32_t) writeDescriptorStatus));

        std::vector<uint8_t> bytes;
        bytes.push_back((uint8_t) descriptorValue);

        auto success = writeDescriptorStatus == GattCommunicationStatus::Success;
        method_channel_->InvokeMethod("OnDescriptorWritten",
            std::make_unique<EncodableValue>(EncodableMap{
                  {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothDeviceAgent.device.BluetoothAddress()))},
                  {"service_uuid", EncodableValue(service)},
                  {"secondary_service_uuid", EncodableValue()},
                  {"characteristic_uuid", EncodableValue(characteristic)},
                  {"descriptor_uuid", EncodableValue("2902")},
                  {"value", EncodableValue(to_hexstring(bytes))},
                  {"success", EncodableValue(success ? 1 : 0)},
                  {"error_string", EncodableValue(success ? "success" : "invalid status")},
                  {"error_code", EncodableValue(success ? 0 : (int32_t) writeDescriptorStatus)}
            }));

        if (bleInputProperty != 0) {
            bluetoothDeviceAgent.valueChangedTokens[characteristic] = gattCharacteristic.ValueChanged({ this, &FlutterBluePlusPlugin::GattCharacteristic_ValueChanged });
        }
        else {
            gattCharacteristic.ValueChanged(std::exchange(bluetoothDeviceAgent.valueChangedTokens[characteristic], {}));
        }
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::ReadValueAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic) {
        auto gattCharacteristic = co_await bluetoothDeviceAgent.GetCharacteristicAsync(service, characteristic);
        auto readValueResult = co_await gattCharacteristic.ReadValueAsync();
        auto bytes = to_bytevc(readValueResult.Value());

        FBPLog(LDEBUG, L"ReadValueAsync " + winrt::to_hstring(characteristic) + L", " + winrt::to_hstring(to_hexstring(bytes)));

        method_channel_->InvokeMethod("OnCharacteristicReceived",
            std::make_unique<EncodableValue>(EncodableMap{
                  {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothDeviceAgent.device.BluetoothAddress()))},
                  {"service_uuid", EncodableValue(service)},
                  {"secondary_service_uuid", EncodableValue()},
                  {"characteristic_uuid", EncodableValue(characteristic)},
                  {"value", EncodableValue(to_hexstring(bytes))},
                  {"success", EncodableValue(1)},
                  {"error_string", EncodableValue("success")},
                  {"error_code", EncodableValue(0)}
            }));
    }

    winrt::fire_and_forget FlutterBluePlusPlugin::WriteValueAsync(BluetoothDeviceAgent& bluetoothDeviceAgent, std::string service, std::string characteristic, std::vector<uint8_t> value, int32_t bleOutputProperty) {
        auto gattCharacteristic = co_await bluetoothDeviceAgent.GetCharacteristicAsync(service, characteristic);
        auto writeOption = bleOutputProperty == 0 ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse;
        auto writeValueStatus = co_await gattCharacteristic.WriteValueAsync(from_bytevc(value), writeOption);
        FBPLog(LDEBUG, L"WriteValueAsync " + winrt::to_hstring(characteristic) + L", " + winrt::to_hstring(to_hexstring(value)) + L", " + winrt::to_hstring((int32_t)writeValueStatus));

        method_channel_->InvokeMethod("OnCharacteristicWritten",
            std::make_unique<EncodableValue>(EncodableMap{
                  {"remote_id", winrt::to_string(formatBluetoothAddress(bluetoothDeviceAgent.device.BluetoothAddress()))},
                  {"service_uuid", EncodableValue(service)},
                  {"secondary_service_uuid", EncodableValue()},
                  {"characteristic_uuid", EncodableValue(characteristic)},
                  {"value", EncodableValue(to_hexstring(value))},
                  {"success", EncodableValue((int32_t)writeValueStatus == 0 ? 1 : 0)},
                  {"error_string", EncodableValue((int32_t)writeValueStatus == 0 ? "success" : "Invalid Status")},
                  {"error_code", EncodableValue((int32_t)writeValueStatus)}
            }));
    }

    void FlutterBluePlusPlugin::GattCharacteristic_ValueChanged(GattCharacteristic sender, GattValueChangedEventArgs args) {
        auto characteristic_uuid = to_uuidstr(sender.Uuid());
        auto service_uuid = to_uuidstr(sender.Service().Uuid());
        auto bytes = to_bytevc(args.CharacteristicValue());
        //FBPLog(LDEBUG, L"GattCharacteristic_ValueChanged " + winrt::to_hstring(characteristic_uuid) + L", " + winrt::to_hstring(service_uuid) + L", " + winrt::to_hstring(to_hexstring(bytes))));

        method_channel_->InvokeMethod("OnCharacteristicReceived",
            std::make_unique<EncodableValue>(EncodableMap{
                  {"remote_id", winrt::to_string(formatBluetoothAddress(sender.Service().Device().BluetoothAddress()))},
                  {"service_uuid", EncodableValue(service_uuid)},
                  {"secondary_service_uuid", EncodableValue()},
                  {"characteristic_uuid", EncodableValue(characteristic_uuid)},
                  {"value", EncodableValue(to_hexstring(bytes))},
                  {"success", EncodableValue(1)},
                  {"error_string", EncodableValue("success")},
                  {"error_code", EncodableValue(0)}
            }));
    }

    void FlutterBluePlusPlugin::FBPLog(LogLevel level, winrt::hstring message) {
        if (level <= logLevel) {
            OutputDebugString((L"[FBP-Win] " + message + L"\n").c_str());
        }
    }
}  // namespace flutter_blue_plus

void FlutterBluePlusPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
    FlutterBluePlusPlugin::RegisterWithRegistrar(
        flutter::PluginRegistrarManager::GetInstance()
        ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}