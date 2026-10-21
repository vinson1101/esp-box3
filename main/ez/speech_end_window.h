#pragma once

// VADNet already confirms speech for 128 ms and silence for 1000 ms in the AFE.
// Consume that endpoint directly instead of adding Bridge's raw-energy tail window.
class SpeechEndWindow {
public:
    void Reset(bool heard_speech = false) { heard_speech_ = heard_speech; }
    bool Push(bool speaking) {
        heard_speech_ = heard_speech_ || speaking;
        return heard_speech_ && !speaking;
    }
    bool HeardSpeech() const { return heard_speech_; }

private:
    bool heard_speech_ = false;
};
