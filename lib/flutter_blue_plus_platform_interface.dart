import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'flutter_blue_plus_method_channel.dart';

abstract class FlutterBluePlusPlatform extends PlatformInterface {
  /// Constructs a FlutterBluePlusPlatform.
  FlutterBluePlusPlatform() : super(token: _token);

  static final Object _token = Object();

  static FlutterBluePlusPlatform _instance = MethodChannelFlutterBluePlus();

  /// The default instance of [FlutterBluePlusPlatform] to use.
  ///
  /// Defaults to [MethodChannelFlutterBluePlus].
  static FlutterBluePlusPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [FlutterBluePlusPlatform] when
  /// they register themselves.
  static set instance(FlutterBluePlusPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
