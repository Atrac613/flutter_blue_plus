import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_blue_plus/flutter_blue_plus_platform_interface.dart';
import 'package:flutter_blue_plus/flutter_blue_plus_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockFlutterBluePlusPlatform
    with MockPlatformInterfaceMixin
    implements FlutterBluePlusPlatform {

  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final FlutterBluePlusPlatform initialPlatform = FlutterBluePlusPlatform.instance;

  test('$MethodChannelFlutterBluePlus is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelFlutterBluePlus>());
  });

  test('getPlatformVersion', () async {
    FlutterBluePlus flutterBluePlusPlugin = FlutterBluePlus();
    MockFlutterBluePlusPlatform fakePlatform = MockFlutterBluePlusPlatform();
    FlutterBluePlusPlatform.instance = fakePlatform;

    expect(await flutterBluePlusPlugin.getPlatformVersion(), '42');
  });
}
