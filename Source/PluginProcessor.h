#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include "EngineRunner.h"
#include "AraSupport.h"

class AIVoiceConverterProcessor : public juce::AudioProcessor,
                                  public juce::AudioProcessorARAExtension,
                                  public juce::ChangeBroadcaster,
                                  private juce::Timer,
                                  private EngineRunner::Listener
{
public:
    static constexpr int kNumStems = 1;

    AIVoiceConverterProcessor();
    ~AIVoiceConverterProcessor() override;

    // ---- AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { releaseResourcesForARA(); }
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AI Voice Converter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override
    {
        double tail = 0.0;
        if (getTailLengthSecondsForARA (tail))
            return tail;
        return 0.0;
    }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- Separacion (hilo de mensajes) ----
    // Devuelve "" si arranco, o un mensaje de error.
    juce::String startSeparation (const juce::File& input, const SeparationOptions&, double startSeconds = 0.0);

    // ---- ARA (hilo de mensajes) ----
    // true si el host nos enlazo por ARA: el plugin lee y reproduce los clips de esta pista
    bool isAraActive() const noexcept { return isPlaybackRenderer(); }
    bool isAraBusy() const noexcept { return araRunning; }
    juce::String getAraError() const { return araError; }
    juce::String getAraStatusText() const;
    // Convierte todos los clips de la pista cuyo audio ya se leyo. Devuelve "" o un error.
    juce::String startAraConversion (const SeparationOptions&);

    // ---- Captura de la pista (hilo de mensajes) ----
    // Graba (mono) lo que entra al plugin mientras el host reproduce. Devuelve "" o un error.
    juce::String startRecording();
    // Termina la captura y guarda el WAV; devuelve el archivo (vacio si no se grabo nada).
    juce::File stopRecording (double& startSeconds);
    bool isRecording() const { return recArmed.load(); }
    double getRecordedSeconds() const;

    // ---- Estado para la interfaz (hilo de mensajes) ----
    bool hasStems() const;
    juce::StringArray getStemNames() const;
    juce::File getStemFile (int slot) const;
    juce::File getExportFile (int slot) const; // copia con nombre "<cancion> - <stem>.wav"
    juce::String getSourceName() const;
    juce::String getAnalysisText() const;      // "120 BPM - Am - 440 Hz"
    juce::String getDeviceText() const;        // "CUDA (RTX ...)" / "CPU"
    float getLevel (int slot) const { return levels[(size_t) slot].load(); }

    // Opciones persistentes de la interfaz (se guardan en el proyecto).
    juce::String getOption (const juce::String& key, const juce::String& def) const;
    void setOption (const juce::String& key, const juce::String& value);

    juce::AudioProcessorValueTreeState apvts;
    EngineRunner engine;

private:
    struct Stem
    {
        juce::String name;
        juce::File file;
        std::unique_ptr<juce::BufferingAudioReader> reader;
    };
    struct StemSet
    {
        std::vector<std::unique_ptr<Stem>> stems;
        double sampleRate = 44100.0;
        juce::int64 length = 0;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static BusesProperties makeBuses();

    void loadStems (const juce::File& dir, const juce::StringArray& names);

    // ARA
    void timerCallback() override;
    std::vector<AraAudioModification*> getAraModifications() const;
    void loadAraOriginals();
    void startNextAra();
    void finishAra (const juce::String& error);
    void handleAraFinished (bool ok, const juce::var& result, const juce::String& error);
    AraAudioPtr loadAudioFile (const juce::File&) const;
    void restoreFromState();
    void applyResult (const juce::var& result, const juce::File& input);
    void readStem (Stem&, juce::int64 start, int count, float* d0, float* d1, juce::int64 length);

    // EngineRunner::Listener
    void engineFinished (EngineRunner::Task, bool ok, const juce::var& result, const juce::String& error) override;

    mutable juce::AudioFormatManager formats;
    juce::TimeSliceThread readAhead { "AI Voice read-ahead" };

    juce::SpinLock setLock;
    std::unique_ptr<StemSet> currentSet; // protegido por setLock

    std::atomic<float>* offsetParam = nullptr;
    std::atomic<float>* wetParam = nullptr;
    std::atomic<float>* dryParam = nullptr;
    std::array<std::atomic<float>, kNumStems> levels {};
    float lastWet = 0.0f, lastDry = 1.0f;
    std::atomic<bool> hasConverted { false };
    std::atomic<double> startSec { 0.0 };   // donde empieza la voz convertida en el proyecto (captura)

    juce::AudioBuffer<float> scratch;   // lectura cruda
    juce::AudioBuffer<float> chunk;     // voz ya remuestreada
    double hostRate = 44100.0;

    // captura
    juce::SpinLock recLock;
    std::vector<float> recBuf;
    std::atomic<bool> recArmed { false };
    std::atomic<juce::int64> recStart { -1 };
    std::atomic<juce::int64> recLength { 0 };

    std::vector<AraAudioModification*> araQueue;
    AraAudioModification* araCurrent = nullptr;
    bool araRunning = false;
    int araTotal = 0, araDone = 0, araTick = 0;
    SeparationOptions araOptions;
    juce::String araError;

    juce::File pendingInput; // archivo que se esta convirtiendo
    double pendingStart = 0.0;
    juce::String pendingError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AIVoiceConverterProcessor)
};
