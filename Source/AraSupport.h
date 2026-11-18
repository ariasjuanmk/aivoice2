#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>
#include <vector>

#if ! JucePlugin_Enable_ARA
 #error "Este proyecto requiere ARA: IS_ARA_EFFECT TRUE en CMakeLists.txt"
#endif

// Audio completo en memoria (la fuente original o su version convertida).
// Se comparte con el hilo de audio solo como shared_ptr<const>, nunca se modifica.
struct AraAudio
{
    juce::AudioBuffer<float> data;   // siempre 2 canales (una fuente mono se duplica)
    double sampleRate = 44100.0;
    juce::int64 length = 0;
};
using AraAudioPtr = std::shared_ptr<const AraAudio>;

// Una "modificacion de audio" de ARA es el audio de un clip tal como lo ve este plugin.
// Guarda el audio original y, si ya se convirtio, la voz convertida.
class AraAudioModification : public juce::ARAAudioModification
{
public:
    AraAudioModification (juce::ARAAudioSource* audioSource,
                          ARA::ARAAudioModificationHostRef hostRef,
                          const juce::ARAAudioModification* optionalModificationToClone);
    ~AraAudioModification() override;

    AraAudioPtr getOriginal() const  { return std::atomic_load (&original); }
    AraAudioPtr getConverted() const { return std::atomic_load (&converted); }
    void setOriginal (AraAudioPtr p);
    void setConverted (AraAudioPtr p);

    // true si el objeto sigue vivo (el host puede borrar clips mientras se convierte)
    static bool isAlive (const AraAudioModification*);

    int loadFailures = 0;   // intentos fallidos de leer el audio (solo hilo de mensajes)

private:
    mutable AraAudioPtr original, converted;
};

// El renderizador pide un bloqueo de lectura para no tocar el modelo mientras el host lo edita.
struct AraLockInterface
{
    virtual ~AraLockInterface() = default;
    virtual juce::ScopedTryReadLock getProcessingLock() = 0;
};

// Reproduce los clips de la pista: voz convertida (si existe) y/o audio original.
class AraPlaybackRenderer final : public juce::ARAPlaybackRenderer
{
public:
    AraPlaybackRenderer (ARA::PlugIn::DocumentController* dc, AraLockInterface& lockInterfaceIn)
        : juce::ARAPlaybackRenderer (dc), lockInterface (lockInterfaceIn) {}

    // El procesador actualiza esto en cada bloque (ganancias lineales y desplazamiento en segundos).
    void setMix (float wetGainIn, float dryGainIn, double offsetSecondsIn) noexcept
    {
        wetGain.store (wetGainIn);
        dryGain.store (dryGainIn);
        offsetSeconds.store (offsetSecondsIn);
    }

    void prepareToPlay (double sampleRateIn, int, int, juce::AudioProcessor::ProcessingPrecision,
                        AlwaysNonRealtime) override
    {
        sampleRate = sampleRateIn;
    }

    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

private:
    AraLockInterface& lockInterface;
    double sampleRate = 44100.0;
    std::atomic<float> wetGain { 1.0f }, dryGain { 0.0f };
    std::atomic<double> offsetSeconds { 0.0 };
};
