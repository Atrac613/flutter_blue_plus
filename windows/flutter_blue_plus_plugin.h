#ifndef FLUTTER_PLUGIN_FLUTTER_BLUE_PLUS_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_BLUE_PLUS_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

namespace flutter_blue_plus {

class FlutterBluePlusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  FlutterBluePlusPlugin();

  virtual ~FlutterBluePlusPlugin();

  // Disallow copy and assign.
  FlutterBluePlusPlugin(const FlutterBluePlusPlugin&) = delete;
  FlutterBluePlusPlugin& operator=(const FlutterBluePlusPlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

}  // namespace flutter_blue_plus

#endif  // FLUTTER_PLUGIN_FLUTTER_BLUE_PLUS_PLUGIN_H_
