#include "AraSupport.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace
{
juce::CriticalSection& aliveLock()
{
    static juce::CriticalSection c;
    return c;
}

std::set<const AraAudioModification*>& aliveSet()
{
    static std::set<const AraAudioModification*> s;
    return s;
}

// Los audios reemplazados se conservan unos segundos: el hilo de audio puede estar
// usando todavia el anterior, y no debe liberarse memoria desde ese hilo.
struct Retired
{
    AraAudioPtr ptr;
    juce::uint32 time;
};

void retire (AraAudioPtr old)
{
    static juce::CriticalSection lock;
    static std::vector<Retired> graveyard;

    const juce::ScopedLock sl (lock);
    const auto now = juce::Time::getMillisecondCounter();
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [now] (const Retired& r) { return now - r.time > 10000; }),
                     graveyard.end());
    if (old != nullptr)
        graveyard.push_back ({ std::move (old), now });
}

// Mezcla 'num' muestras de 'a' en 'out' (desde destStart), empezando en el instante
// srcStartSeconds de la fuente, con interpolacion lineal si los sample rates difieren.
void mixInto (juce::AudioBuffer<float>& out, int destStart, int num, const AraAudio& a,
              double srcStartSeconds, double hostRate, float gain)
{
    if (num <= 0 || a.length < 2 || gain <= 0.0f || hostRate <= 0.0)
        return;

    const double ratio = a.sampleRate / hostRate;
    const double pos = srcStartSeconds * a.sampleRate;
    const float* s0 = a.data.getReadPointer (0);
    const float* s1 = a.data.getReadPointer (a.data.getNumChannels() > 1 ? 1 : 0);
    float* d0 = out.getWritePointer (0, destStart);
    float* d1 = out.getNumChannels() > 1 ? out.getWritePointer (1, destStart) : nullptr;

    for (int i = 0; i < num; ++i)
    {
        const double p = pos + (double) i * ratio;
        const auto i0 = (juce::int64) std::floor (p);
        if (i0 < 0 || i0 + 1 >= a.length)
            continue; // fuera del audio: silencio
        const float t = (float) (p - (double) i0);
        d0[i] += gain * (s0[i0] + t * (s0[i0 + 1] - s0[i0]));
        if (d1 != nullptr)
            d1[i] += gain * (s1[i0] + t * (s1[i0 + 1] - s1[i0]));
    }
}
} // namespace

// --------------------------------------------------------------------------
AraAudioModification::AraAudioModification (juce::ARAAudioSource* audioSource,
                                            ARA::ARAAudioModificationHostRef hostRef,
                                            const juce::ARAAudioModification* optionalModificationToClone)
    : juce::ARAAudioModification (audioSource, hostRef, optionalModificationToClone)
{
    const juce::ScopedLock sl (aliveLock());
    aliveSet().insert (this);
}

AraAudioModification::~AraAudioModification()
{
    const juce::ScopedLock sl (aliveLock());
    aliveSet().erase (this);
}

bool AraAudioModification::isAlive (const AraAudioModification* m)
{
    const juce::ScopedLock sl (aliveLock());
    return aliveSet().count (m) > 0;
}

void AraAudioModification::setOriginal (AraAudioPtr p)
{
    retire (std::atomic_exchange (&original, std::move (p)));
}

void AraAudioModification::setConverted (AraAudioPtr p)
{
    retire (std::atomic_exchange (&converted, std::move (p)));
}

// --------------------------------------------------------------------------
// Renderizador de reproduccion (hilo de audio)
// --------------------------------------------------------------------------
bool AraPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::AudioProcessor::Realtime,
                                        const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    buffer.clear();

    const auto lock = lockInterface.getProcessingLock();
    if (! lock.isLocked() || ! positionInfo.getIsPlaying() || sampleRate <= 0.0)
        return true;

    const int numSamples = buffer.getNumSamples();
    const juce::int64 blockStart = positionInfo.getTimeInSamples().orFallback (juce::int64 (0));
    const juce::Range<juce::int64> blockRange (blockStart, blockStart + (juce::int64) numSamples);

    const float wet = wetGain.load();
    const float dry = dryGain.load();
    const double offset = offsetSeconds.load();

    for (auto* region : getPlaybackRegions())
    {
        const auto regionRange = region->getSampleRange (sampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
        const auto render = blockRange.getIntersectionWith (regionRange);
        if (render.isEmpty())
            continue;

        auto* mod = region->getAudioModification<AraAudioModification>();
        if (mod == nullptr)
            continue;

        const auto original = mod->getOriginal();
        const auto converted = mod->getConverted();

        const int destStart = (int) (render.getStart() - blockStart);
        const int num = (int) render.getLength();

        // instante de la fuente que corresponde a la primera muestra renderizada
        const double songTime = (double) render.getStart() / sampleRate;
        const double sourceTime = songTime - region->getStartInPlaybackTime()
                                  + region->getStartInAudioModificationTime();

        if (converted != nullptr)
        {
            mixInto (buffer, destStart, num, *converted, sourceTime - offset, sampleRate, wet);
            if (original != nullptr)
                mixInto (buffer, destStart, num, *original, sourceTime, sampleRate, dry);
        }
        else if (original != nullptr)
        {
            mixInto (buffer, destStart, num, *original, sourceTime, sampleRate, 1.0f);
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// Controlador de documento ARA
// --------------------------------------------------------------------------
class AraDocumentController final : public juce::ARADocumentControllerSpecialisation,
                                    private AraLockInterface
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

protected:
    // El host edita el modelo: el hilo de audio no debe leerlo mientras tanto
    void willBeginEditing (juce::ARADocument*) override { processBlockLock.enterWrite(); }
    void didEndEditing (juce::ARADocument*) override { processBlockLock.exitWrite(); }

    juce::ARAAudioModification* doCreateAudioModification (juce::ARAAudioSource* audioSource,
                                                           ARA::ARAAudioModificationHostRef hostRef,
                                                           const juce::ARAAudioModification* optionalModificationToClone) noexcept override
    {
        return new AraAudioModification (audioSource, hostRef, optionalModificationToClone);
    }

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override
    {
        return new AraPlaybackRenderer (getDocumentController(), *this);
    }

    // Esta version no guarda nada en el archivo del proyecto del DAW: al reabrirlo,
    // "Convertir" vuelve a aplicar la conversion al instante gracias a la cache.
    bool doRestoreObjectsFromStream (juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override
    {
        return true;
    }

    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override
    {
        return true;
    }

private:
    juce::ScopedTryReadLock getProcessingLock() override
    {
        return juce::ScopedTryReadLock (processBlockLock);
    }

    juce::ReadWriteLock processBlockLock;
};

// Punto de entrada que busca el wrapper VST3 de JUCE para ofrecer ARA al host
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<AraDocumentController>();
}
