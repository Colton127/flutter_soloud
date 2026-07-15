import 'package:flutter_soloud/src/enums.dart';
import 'package:flutter_soloud/src/exceptions/exceptions.dart';
import 'package:test/test.dart';

void main() {
  test('PlayerErrors value corresponds to its position', () {
    for (final error in PlayerErrors.values) {
      expect(
        error.value,
        PlayerErrors.values.indexOf(error),
        reason:
            'The value of $error is ${error.value} '
            'but its position in the PlayerErrors enum is '
            '${PlayerErrors.values.indexOf(error)}. '
            'This makes code such as `final error = PlayerErrors.values[ret];` '
            'invalid.',
      );
    }
  });

  test('audio start errors map to precise exception types', () {
    expect(
      SoLoudCppException.fromPlayerError(PlayerErrors.audioDeviceFailedToStart),
      isA<SoLoudAudioDeviceFailedToStartCppException>(),
    );
    expect(
      SoLoudCppException.fromPlayerError(PlayerErrors.failedToStartPlayback),
      isA<SoLoudFailedToStartPlaybackCppException>(),
    );
  });
}
