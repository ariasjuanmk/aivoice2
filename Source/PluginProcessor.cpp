#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Paths.h"

namespace
{
constexpr int kChunk = 2048;       // muestras del host procesadas por pasada
constexpr int kScratchCap = 16384; // muestras crudas maximas por pasada
constexpr double kMaxRecordSeconds = 600.0; // limite de captura (10 min, mono)

// WAV float32 mono escrito a mano (formato simple y seguro, lo lee cualquier libreria)
bool writeWavFloat32Mono (const juce::File& file, const float* data, juce::int64 n, int rate)
{
    juce::FileOutputStream out (file);
    if (out.failedToOpen())
        return false;
    out.setPosition (0);
    out.truncate();
    const juce::uint32 bytes = (juce::uint32) (n * 4);
    auto w32 = [&] (juce::uint32 v) { out.writeInt (v); };
    auto w16 = [&] (juce::uint16 v) { out.writeShort ((short) v); };
    out.write ("RIFF", 4);  w32 (36 + bytes);
    out.write ("WAVE", 4);  out.write ("fmt ", 4);
    w32 (16); w16 (3); w16 (1); w32 ((juce::uint32) rate); w32 ((juce::uint32) rate * 4); w16 (4); w16 (32);
    out.write ("data", 4);  w32 (bytes);
    out.write (data, (size_t) bytes);
    out.flush();
    return ! out.getStatus().failed();
}
} // namespace

juce::AudioProcessor::BusesProperties AIVoiceConverterProcessor::makeBuses()
{
    return BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

juce::AudioProcessorValueTreeState::ParameterLayout AIVoiceConverterProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "offset", 1 }, "Offset (s)",
        NormalisableRange<float> (-600.0f, 600.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "wet", 1 }, "Voz convertida (dB)",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "dry", 1 }, "Voz original (dB)",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), -60.0f));
    return layout;
}

AIVoiceConverterProcessor::AIVoiceConverterProcessor()
    : AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "AIVoiceConverter", createLayout())
{
    formats.registerBasicFormats();

    offsetParam = apvts.getRawParameterValue ("offset");
    wetParam = apvts.getRawParameterValue ("wet");
    dryParam = apvts.getRawParameterValue ("dry");
    for (auto& l : levels)
        l = 0.0f;

    scratch.setSize (2, kScratchCap);
    chunk.setSize (2, kChunk);

    engine.addListener (this);
    startTimer (100);
}

AIVoiceConverterProcessor::~AIVoiceConverterProcessor()
{
    stopTimer();
    engine.removeListener (this);
    engine.cancel();
    {
        const juce::SpinLock::ScopedLockType lock (setLock);
        currentSet.reset(); // los readers se desregistran del hilo antes de pararlo
    }
    readAhead.stopThread (2000);
}

bool AIVoiceConverterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
}

void AIVoiceConverterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
    hostRate = sampleRate;
    lastWet = 0.0f;
    lastDry = 1.0f;
}

// --------------------------------------------------------------------------
// Audio
// --------------------------------------------------------------------------
void AIVoiceConverterProcessor::readStem (Stem& s, juce::int64 start, int count,
                                        float* d0, float* d1, juce::int64 length)
{
    juce::FloatVectorOperations::clear (d0, count);
    juce::FloatVectorOperations::clear (d1, count);

    const juce::int64 from = juce::jmax<juce::int64> (0, start);
    const juce::int64 to = juce::jmin<juce::int64> (length, start + count);
    if (to <= from)
        return;

    const int destOffset = (int) (from - start);
    const int num = (int) (to - from);
    float* dest[2] = { d0 + destOffset, d1 + destOffset };
    s.reader->read (dest, 2, from, num); // sin bloquear: si no esta en buffer queda silencio
}

void AIVoiceConverterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (2, buffer.getNumChannels());

    // ---- modo ARA: el host nos da los clips de la pista y nosotros los reproducimos ----
    if (isPlaybackRenderer())
    {
        if (auto* renderer = getPlaybackRenderer<AraPlaybackRenderer>())
            renderer->setMix (juce::Decibels::decibelsToGain (wetParam->load(), -60.0f),
                              juce::Decibels::decibelsToGain (dryParam->load(), -60.0f),
                              (double) offsetParam->load());

        const auto realtime = isNonRealtime() ? juce::AudioProcessor::Realtime::no
                                              : juce::AudioProcessor::Realtime::yes;
        if (! processBlockForARA (buffer, realtime, getPlayHead()))
            processBlockBypassed (buffer, midi);

        const float peak = numCh > 0 ? juce::jmax (buffer.getMagnitude (0, 0, numSamples),
                                                   buffer.getMagnitude (numCh - 1, 0, numSamples))
                                     : 0.0f;
        levels[0].store (juce::jmax (peak, levels[0].load() * 0.9f));
        return;
    }

    // posicion del host
    bool playing = false;
    juce::int64 hostPos = 0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto t = pos->getTimeInSamples())
                hostPos = *t;
            else if (auto sec = pos->getTimeInSeconds())
                hostPos = (juce::int64) std::llround (*sec * hostRate);
        }
    }

    // 1) captura (lo que entra, en mono), antes de tocar el buffer
    if (playing && recArmed.load() && numCh > 0)
    {
        const juce::SpinLock::ScopedTryLockType lock (recLock);
        if (lock.isLocked() && recArmed.load() && ! recBuf.empty())
        {
            juce::int64 first = recStart.load();
            if (first < 0)
            {
                first = hostPos;
                recStart.store (first);
            }
            const auto cap = (juce::int64) recBuf.size();
            const float* in0 = buffer.getReadPointer (0);
            const float* in1 = buffer.getReadPointer (numCh > 1 ? 1 : 0);
            juce::int64 last = recLength.load();
            for (int i = 0; i < numSamples; ++i)
            {
                const juce::int64 idx = hostPos + i - first;
                if (idx >= 0 && idx < cap)
                {
                    recBuf[(size_t) idx] = 0.5f * (in0[i] + in1[i]);
                    last = juce::jmax (last, idx + 1);
                }
            }
            recLength.store (last);
        }
    }

    // 2) original (dry): sin voz convertida o mientras se graba, la pista pasa tal cual
    const bool passThrough = recArmed.load() || ! hasConverted.load();
    const float dryTarget = passThrough ? 1.0f
                                        : juce::Decibels::decibelsToGain (dryParam->load(), -60.0f);
    for (int ch = 0; ch < numCh; ++ch)
        buffer.applyGainRamp (ch, 0, numSamples, lastDry, dryTarget);
    lastDry = dryTarget;

    // 3) voz convertida (wet)
    const float wetTarget = (passThrough || ! playing) ? 0.0f
                                                       : juce::Decibels::decibelsToGain (wetParam->load(), -60.0f);
    if (! playing)
    {
        for (auto& l : levels)
            l.store (l.load() * 0.8f);
        lastWet = 0.0f;
        return;
    }

    const juce::SpinLock::ScopedTryLockType lock (setLock);
    if (! lock.isLocked() || currentSet == nullptr || currentSet->stems.empty())
        return;

    auto& set = *currentSet;
    auto& stem = *set.stems[0];
    const double stemRate = set.sampleRate;
    const double ratio = stemRate / hostRate;
    const bool sameRate = std::abs (ratio - 1.0) < 1.0e-9;
    const double offsetSec = (double) offsetParam->load() + startSec.load();

    // chunk maximo para no pasar de kScratchCap muestras crudas
    const int maxChunk = juce::jlimit (1, kChunk, (int) ((kScratchCap - 16) / juce::jmax (1.0, ratio)));
    const float wetFrom = lastWet;

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = juce::jmin (maxChunk, numSamples - start);
        const juce::int64 blockPos = hostPos + start;
        float* c0 = chunk.getWritePointer (0);
        float* c1 = chunk.getWritePointer (1);

        if (sameRate)
        {
            const juce::int64 src = blockPos - (juce::int64) std::llround (offsetSec * hostRate);
            readStem (stem, src, n, c0, c1, set.length);
        }
        else
        {
            // interpolacion lineal sobre la posicion absoluta (continua entre bloques)
            const double srcPos = ((double) blockPos / hostRate - offsetSec) * stemRate;
            const auto base = (juce::int64) std::floor (srcPos);
            const double frac = srcPos - (double) base;
            const int need = juce::jmin (kScratchCap, (int) std::ceil ((double) n * ratio + frac) + 2);
            float* s0 = scratch.getWritePointer (0);
            float* s1 = scratch.getWritePointer (1);
            readStem (stem, base, need, s0, s1, set.length);
            for (int k = 0; k < n; ++k)
            {
                const double p = frac + (double) k * ratio;
                const int i0 = (int) p;
                const float t = (float) (p - (double) i0);
                c0[k] = s0[i0] + t * (s0[i0 + 1] - s0[i0]);
                c1[k] = s1[i0] + t * (s1[i0 + 1] - s1[i0]);
            }
        }

        const float peak = juce::jmax (chunk.getMagnitude (0, 0, n), chunk.getMagnitude (1, 0, n));
        levels[0].store (juce::jmax (peak, levels[0].load() * 0.9f));

        // rampa de ganancia a lo largo del bloque entero (sin saltos entre chunks)
        const float g0 = wetFrom + (wetTarget - wetFrom) * (float) start / (float) numSamples;
        const float g1 = wetFrom + (wetTarget - wetFrom) * (float) (start + n) / (float) numSamples;
        for (int ch = 0; ch < numCh; ++ch)
            buffer.addFromWithRamp (ch, start, chunk.getReadPointer (ch), n, g0, g1);
    }
    lastWet = wetTarget;
}

// --------------------------------------------------------------------------
// Captura de la pista
// --------------------------------------------------------------------------
juce::String AIVoiceConverterProcessor::startRecording()
{
    if (engine.isBusy())
        return "El motor esta ocupado.";
    if (hostRate < 8000.0)
        return "El host no inicializo el audio todavia.";
    {
        const juce::SpinLock::ScopedLockType lock (recLock);
        recArmed.store (false);
        recBuf.assign ((size_t) (kMaxRecordSeconds * hostRate), 0.0f);
        recStart.store (-1);
        recLength.store (0);
    }
    recArmed.store (true);
    return {};
}

double AIVoiceConverterProcessor::getRecordedSeconds() const
{
    return (double) recLength.load() / juce::jmax (1.0, hostRate);
}

juce::File AIVoiceConverterProcessor::stopRecording (double& startSeconds)
{
    juce::int64 len = 0, first = -1;
    {
        const juce::SpinLock::ScopedLockType lock (recLock);
        recArmed.store (false);
        len = recLength.load();
        first = recStart.load();
    }
    startSeconds = 0.0;
    if (len <= 0 || first < 0)
    {
        recBuf.clear();
        recBuf.shrink_to_fit();
        return {};
    }

    stempaths::capturesDir().createDirectory();
    const auto file = stempaths::capturesDir().getChildFile (
        "captura_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S") + ".wav");
    const bool ok = writeWavFloat32Mono (file, recBuf.data(), len, (int) std::lround (hostRate));
    recBuf.clear();
    recBuf.shrink_to_fit();
    if (! ok)
        return {};
    startSeconds = (double) first / hostRate;
    return file;
}

// --------------------------------------------------------------------------
// Carga de stems
// --------------------------------------------------------------------------
void AIVoiceConverterProcessor::loadStems (const juce::File& dir, const juce::StringArray& names)
{
    auto set = std::make_unique<StemSet>();
    for (const auto& name : names)
    {
        const auto file = dir.getChildFile (name + ".wav");
        if (! file.existsAsFile())
            continue;
        std::unique_ptr<juce::AudioFormatReader> raw (formats.createReaderFor (file));
        if (raw == nullptr || raw->numChannels < 1)
            continue;

        set->sampleRate = raw->sampleRate;
        set->length = juce::jmax (set->length, raw->lengthInSamples);

        auto stem = std::make_unique<Stem>();
        stem->name = name;
        stem->file = file;
        stem->reader = std::make_unique<juce::BufferingAudioReader> (raw.release(), readAhead, 1 << 17);
        stem->reader->setReadTimeout (0);
        set->stems.push_back (std::move (stem));
    }

    if (! readAhead.isThreadRunning())
        readAhead.startThread (juce::Thread::Priority::normal);

    std::unique_ptr<StemSet> old;
    {
        const juce::SpinLock::ScopedLockType lock (setLock);
        old = std::move (currentSet);
        currentSet = std::move (set);
    }
    hasConverted.store (currentSet != nullptr && ! currentSet->stems.empty());
    old.reset(); // se libera fuera del lock
}

bool AIVoiceConverterProcessor::hasStems() const
{
    return apvts.state.getProperty ("stemNames").toString().isNotEmpty();
}

juce::StringArray AIVoiceConverterProcessor::getStemNames() const
{
    juce::StringArray out;
    out.addTokens (apvts.state.getProperty ("stemNames").toString(), "|", "");
    out.removeEmptyStrings();
    return out;
}

juce::File AIVoiceConverterProcessor::getStemFile (int slot) const
{
    auto names = getStemNames();
    if (slot < 0 || slot >= names.size())
        return {};
    return juce::File (apvts.state.getProperty ("stemsDir").toString()).getChildFile (names[slot] + ".wav");
}

juce::File AIVoiceConverterProcessor::getExportFile (int slot) const
{
    const auto src = getStemFile (slot);
    if (! src.existsAsFile())
        return {};

    const auto exportDir = src.getParentDirectory().getParentDirectory().getChildFile ("export");
    exportDir.createDirectory();
    const auto song = juce::File::createLegalFileName (getSourceName());
    const auto dst = exportDir.getChildFile (song + " - " + src.getFileNameWithoutExtension() + ".wav");
    if (! dst.existsAsFile() || dst.getSize() != src.getSize())
        src.copyFileTo (dst);
    return dst;
}

juce::String AIVoiceConverterProcessor::getSourceName() const
{
    return apvts.state.getProperty ("sourceName").toString();
}

juce::String AIVoiceConverterProcessor::getAnalysisText() const
{
    const int bpm = (int) apvts.state.getProperty ("bpm", 0);
    const auto key = apvts.state.getProperty ("key").toString();
    const int hz = (int) apvts.state.getProperty ("hz", 0);
    juce::StringArray parts;
    if (bpm > 0) parts.add (juce::String (bpm) + " BPM");
    if (key.isNotEmpty()) parts.add (key);
    if (hz > 0) parts.add (juce::String (hz) + " Hz");
    return parts.joinIntoString (" - ");
}

juce::String AIVoiceConverterProcessor::getDeviceText() const
{
    const auto dev = apvts.state.getProperty ("device").toString();
    const auto gpu = apvts.state.getProperty ("gpu").toString();
    if (dev == "cuda")
        return "CUDA" + (gpu.isNotEmpty() ? " (" + gpu + ")" : juce::String());
    return dev.isNotEmpty() ? dev.toUpperCase() : juce::String();
}

juce::String AIVoiceConverterProcessor::getOption (const juce::String& key, const juce::String& def) const
{
    return apvts.state.getProperty ("opt_" + key, def).toString();
}

void AIVoiceConverterProcessor::setOption (const juce::String& key, const juce::String& value)
{
    apvts.state.setProperty ("opt_" + key, value, nullptr);
}

// --------------------------------------------------------------------------
// Separacion
// --------------------------------------------------------------------------
juce::String AIVoiceConverterProcessor::startSeparation (const juce::File& input, const SeparationOptions& opts,
                                                         double startSeconds)
{
    if (! input.existsAsFile())
        return "El archivo no existe.";
    if (engine.isBusy())
        return "El motor esta ocupado.";
    if (! juce::File (opts.modelPath).existsAsFile())
        return "Elegi un modelo de voz (.pth).";
    if (! engine.getInfo().ready())
        return "El motor no esta listo. Usa el menu \"Motor\" para instalarlo o elegir Python.";

    // la carpeta de cache depende del archivo y de las opciones: si ya existe, no se repite el trabajo
    const auto hash = juce::String::toHexString (
        (input.getFullPathName() + "|" + juce::String (input.getSize()) + "|"
         + juce::String (input.getLastModificationTime().toMilliseconds()) + "|" + opts.toKey()).hashCode64());
    auto name = juce::File::createLegalFileName (input.getFileNameWithoutExtension()).substring (0, 40);
    const auto outDir = stempaths::cacheDir().getChildFile (name + "_" + hash);

    pendingInput = input;
    pendingStart = startSeconds;

    const auto cached = outDir.getChildFile ("result.json");
    if (cached.existsAsFile())
    {
        const auto result = juce::JSON::parse (cached.loadFileAsString());
        if (result.isObject() && juce::File (result["stems_dir"].toString()).isDirectory())
        {
            applyResult (result, input);
            return {};
        }
    }

    if (! engine.startSeparate (input, outDir, opts))
        return "No se pudo iniciar el motor.";
    return {};
}

void AIVoiceConverterProcessor::applyResult (const juce::var& result, const juce::File& input)
{
    const juce::File dir (result["stems_dir"].toString());
    juce::StringArray names;
    if (auto* arr = result["stems"].getArray())
        for (const auto& n : *arr)
            names.add (n.toString());

    auto& st = apvts.state;
    st.setProperty ("stemsDir", dir.getFullPathName(), nullptr);
    st.setProperty ("stemNames", names.joinIntoString ("|"), nullptr);
    st.setProperty ("sourceName", input.getFileNameWithoutExtension(), nullptr);
    st.setProperty ("bpm", (int) result["bpm"], nullptr);
    st.setProperty ("key", result["key"].toString(), nullptr);
    st.setProperty ("hz", (int) result["hz"], nullptr);
    st.setProperty ("device", result["device"].toString(), nullptr);
    st.setProperty ("startSec", pendingStart, nullptr);
    st.setProperty ("model", result["model"].toString(), nullptr);
    startSec.store (pendingStart);
    st.setProperty ("gpu", result["gpu"].toString(), nullptr);

    loadStems (dir, names);
    sendChangeMessage();
}

void AIVoiceConverterProcessor::engineFinished (EngineRunner::Task task, bool ok, const juce::var& result,
                                              const juce::String& error)
{
    if (araRunning && task == EngineRunner::Task::separate)
    {
        handleAraFinished (ok, result, error);
        return;
    }
    if (task == EngineRunner::Task::separate && ok)
        applyResult (result, pendingInput);
}

// --------------------------------------------------------------------------
// ARA: leer los clips de la pista, convertirlos y reproducirlos
// --------------------------------------------------------------------------
std::vector<AraAudioModification*> AIVoiceConverterProcessor::getAraModifications() const
{
    std::vector<AraAudioModification*> out;
    if (auto* renderer = getPlaybackRenderer<AraPlaybackRenderer>())
        for (auto* region : renderer->getPlaybackRegions())
            if (auto* mod = region->getAudioModification<AraAudioModification>())
                if (std::find (out.begin(), out.end(), mod) == out.end())
                    out.push_back (mod);
    return out;
}

void AIVoiceConverterProcessor::timerCallback()
{
    if (! isPlaybackRenderer())
        return;

    // tras el final de un trabajo el motor puede tardar un instante en quedar libre
    if (araRunning && araCurrent == nullptr)
        startNextAra();

    if (++araTick % 10 == 0)
        loadAraOriginals();
}

// Lee (de a un clip por vez) el audio original de los clips de la pista.
void AIVoiceConverterProcessor::loadAraOriginals()
{
    constexpr juce::int64 kMaxSamples = 400000000; // ~ 3 GB como maximo por clip: no se intenta mas
    constexpr int kBlock = 1 << 16;

    for (auto* mod : getAraModifications())
    {
        if (mod->getOriginal() != nullptr || mod->loadFailures >= 6)
            continue;

        auto* source = mod->getAudioSource<juce::ARAAudioSource>();
        if (source == nullptr)
            continue;

        juce::ARAAudioSourceReader reader (source);
        if (reader.lengthInSamples <= 0 || reader.numChannels < 1 || reader.sampleRate <= 0.0
            || reader.lengthInSamples > kMaxSamples)
        {
            ++mod->loadFailures;   // el host todavia no habilito el acceso al audio, o es demasiado grande
            continue;
        }

        auto audio = std::make_shared<AraAudio>();
        audio->sampleRate = reader.sampleRate;
        audio->length = reader.lengthInSamples;
        audio->data.setSize (2, (int) audio->length);

        bool ok = true;
        for (juce::int64 pos = 0; pos < audio->length && ok; pos += kBlock)
        {
            const int n = (int) juce::jmin<juce::int64> (kBlock, audio->length - pos);
            ok = reader.read (&audio->data, (int) pos, n, pos, true, true);
        }
        if (! ok)
        {
            ++mod->loadFailures;
            continue;
        }
        if (reader.numChannels < 2)
            audio->data.copyFrom (1, 0, audio->data, 0, 0, (int) audio->length);

        mod->setOriginal (std::move (audio));
        sendChangeMessage();
        break; // uno por pasada, para no congelar la interfaz
    }
}

AraAudioPtr AIVoiceConverterProcessor::loadAudioFile (const juce::File& file) const
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > 400000000)
        return nullptr;

    auto audio = std::make_shared<AraAudio>();
    audio->sampleRate = reader->sampleRate;
    audio->length = reader->lengthInSamples;
    audio->data.setSize (2, (int) audio->length);
    if (! reader->read (&audio->data, 0, (int) audio->length, 0, true, true))
        return nullptr;
    if (reader->numChannels < 2)
        audio->data.copyFrom (1, 0, audio->data, 0, 0, (int) audio->length);
    return audio;
}

juce::String AIVoiceConverterProcessor::getAraStatusText() const
{
    int clips = 0, ready = 0, converted = 0;
    for (auto* mod : getAraModifications())
    {
        ++clips;
        if (mod->getOriginal() != nullptr)
            ++ready;
        if (mod->getConverted() != nullptr)
            ++converted;
    }

    if (araRunning)
        return "ARA: convirtiendo clip " + juce::String (juce::jmin (araDone + 1, araTotal)) + " de " + juce::String (araTotal) + "...";
    if (clips == 0)
        return "ARA activo, pero esta pista no tiene clips todavia.";

    juce::String text = "ARA activo: " + juce::String (clips) + (clips == 1 ? " clip" : " clips")
                        + " en esta pista (" + juce::String (ready) + " leidos, "
                        + juce::String (converted) + " convertidos).";
    if (ready < clips)
        text += "\nLeyendo el audio de los clips...";
    else if (converted == 0)
        text += "\nElegi el modelo y toca \"Convertir clips de la pista\".";
    return text;
}

juce::String AIVoiceConverterProcessor::startAraConversion (const SeparationOptions& opts)
{
    if (! isPlaybackRenderer())
        return "ARA no esta activo en esta pista.";
    if (araRunning || engine.isBusy())
        return "El motor esta ocupado.";
    if (! juce::File (opts.modelPath).existsAsFile())
        return "Elegi un modelo de voz (.pth).";
    if (! engine.getInfo().ready())
        return "El motor no esta listo. Usa el menu \"Motor\" para instalarlo o elegir Python.";

    araQueue.clear();
    for (auto* mod : getAraModifications())
        if (mod->getOriginal() != nullptr)
            araQueue.push_back (mod);
    if (araQueue.empty())
        return "Todavia no se leyo el audio de los clips. Espera unos segundos (el host debe permitir el acceso al audio).";

    araOptions = opts;
    araTotal = (int) araQueue.size();
    araDone = 0;
    araError = {};
    araCurrent = nullptr;
    araRunning = true;
    startNextAra();
    sendChangeMessage();
    return {};
}

void AIVoiceConverterProcessor::finishAra (const juce::String& error)
{
    araRunning = false;
    araCurrent = nullptr;
    araQueue.clear();
    araError = error;
    sendChangeMessage();
}

void AIVoiceConverterProcessor::startNextAra()
{
    while (! araQueue.empty())
    {
        auto* mod = araQueue.front();
        if (! AraAudioModification::isAlive (mod))
        {
            araQueue.erase (araQueue.begin()); // el usuario borro el clip mientras tanto
            ++araDone;
            continue;
        }
        const auto original = mod->getOriginal();
        if (original == nullptr)
        {
            araQueue.erase (araQueue.begin());
            ++araDone;
            continue;
        }

        // la cache depende del contenido del audio y de las opciones
        // hash FNV-1a de 64 bits del contenido (no hace falta el modulo juce_cryptography)
        juce::uint64 fnv = 1469598103934665603ULL;
        {
            const auto* bytes = reinterpret_cast<const juce::uint8*> (original->data.getReadPointer (0));
            const size_t n = (size_t) original->length * sizeof (float);
            for (size_t i = 0; i < n; ++i)
            {
                fnv ^= bytes[i];
                fnv *= 1099511628211ULL;
            }
        }
        const auto hash = juce::String::toHexString ((juce::int64) fnv) + "_"
                          + juce::String::toHexString (araOptions.toKey().hashCode64());
        const auto outDir = stempaths::cacheDir().getChildFile ("ara_" + hash);

        const auto convertedFile = outDir.getChildFile ("stems").getChildFile ("converted.wav");
        if (convertedFile.existsAsFile())
        {
            if (auto audio = loadAudioFile (convertedFile))
            {
                mod->setConverted (std::move (audio));
                araQueue.erase (araQueue.begin());
                ++araDone;
                continue;
            }
        }

        // escribir el audio (mono) para el motor
        outDir.createDirectory();
        const auto sourceFile = outDir.getChildFile ("source.wav");
        {
            std::vector<float> mono ((size_t) original->length);
            const float* l = original->data.getReadPointer (0);
            const float* r = original->data.getReadPointer (1);
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] = 0.5f * (l[i] + r[i]);
            if (! writeWavFloat32Mono (sourceFile, mono.data(), original->length, (int) std::lround (original->sampleRate)))
            {
                finishAra ("No se pudo escribir el audio temporal en " + outDir.getFullPathName());
                return;
            }
        }

        if (! engine.startSeparate (sourceFile, outDir, araOptions))
        {
            araCurrent = nullptr;   // motor aun ocupado: el temporizador reintenta
            return;
        }
        araCurrent = mod;
        return;
    }

    finishAra ({});
}

void AIVoiceConverterProcessor::handleAraFinished (bool ok, const juce::var& result, const juce::String& error)
{
    auto* mod = araCurrent;
    araCurrent = nullptr;

    if (! ok)
    {
        finishAra (error.isNotEmpty() ? error : juce::String ("La conversion fallo."));
        return;
    }

    const auto file = juce::File (result["stems_dir"].toString()).getChildFile ("converted.wav");
    if (mod != nullptr && AraAudioModification::isAlive (mod))
    {
        auto audio = loadAudioFile (file);
        if (audio == nullptr)
        {
            finishAra ("No se pudo leer el audio convertido: " + file.getFullPathName());
            return;
        }
        mod->setConverted (std::move (audio));
    }

    if (! araQueue.empty())
        araQueue.erase (araQueue.begin());
    ++araDone;
    sendChangeMessage();
    startNextAra();
}

// --------------------------------------------------------------------------
// Estado del proyecto
// --------------------------------------------------------------------------
void AIVoiceConverterProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void AIVoiceConverterProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            restoreFromState();
        }
}

void AIVoiceConverterProcessor::restoreFromState()
{
    startSec.store ((double) apvts.state.getProperty ("startSec", 0.0));
    const juce::File dir (apvts.state.getProperty ("stemsDir").toString());
    const auto names = getStemNames();
    if (dir.isDirectory() && names.size() > 0)
        loadStems (dir, names);
    sendChangeMessage();
}

juce::AudioProcessorEditor* AIVoiceConverterProcessor::createEditor()
{
    return new AIVoiceConverterEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AIVoiceConverterProcessor();
}
