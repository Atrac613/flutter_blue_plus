#include "include/flutter_blue_plus/flutter_blue_plus_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "flutter_blue_plus_plugin.h"

void FlutterBluePlusPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  flutter_blue_plus::FlutterBluePlusPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
