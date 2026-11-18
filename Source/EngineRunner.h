#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>

// Resultado de "rvc_engine.py probe" para un interprete de Python concreto.
struct EngineInfo
{
    bool valid = false;        // se pudo ejecutar y respondio
    bool pythonOk = false;     // Python 3.10 exacto
    bool rvc = false;          // rvc-python instalado
    bool soundfile = false;
    bool cuda = false;         // torch.cuda.is_available()
    bool nvidiaPresent = false;// hay nvidia-smi en el PATH
    bool ffmpeg = false;
    juce::String pythonVersion, torchVersion, gpuName;
    juce::StringArray command; // como invocar este Python (p. ej. {"py","-3"})

    bool ready() const { return valid && rvc && soundfile; }

    // Mayor es mejor: primero CUDA, despues CPU, despues "solo Python".
    int score() const
    {
        if (! valid) return 0;
        if (ready() && cuda) return 4;
        if (ready()) return 3;
        return pythonOk ? 1 : 0;
    }
};

struct SeparationOptions
{
    juce::String modelPath;               // .pth del modelo RVC
    juce::String indexPath;               // .index (opcional)
    int pitch = 0;                        // semitonos
    juce::String f0 = "rmvpe";            // rmvpe | harvest | crepe | pm
    double indexRate = 0.5;
    double protect = 0.33;
    int filterRadius = 3;
    double rmsMix = 1.0;                  // 1 = envolvente de volumen del modelo, 0 = la del original
    juce::String device = "auto";         // auto (CUDA si hay, sino CPU) | cuda | cpu

    // Todo lo que cambia el resultado (el dispositivo no).
    juce::String toKey() const
    {
        const juce::File m (modelPath), i (indexPath);
        return modelPath + "|" + juce::String (m.getSize()) + "|" + juce::String (m.getLastModificationTime().toMilliseconds())
               + "|" + indexPath + "|" + juce::String (i.getSize()) + "|" + juce::String (pitch) + "|" + f0
               + "|" + juce::String (indexRate, 3) + "|" + juce::String (protect, 3) + "|" + juce::String (filterRadius)
               + "|" + juce::String (rmsMix, 3);
    }
};

// Ejecuta el motor Python en un hilo aparte y avisa por el hilo de mensajes.
class EngineRunner : private juce::Thread
{
public:
    enum class Task { none, probe, separate, setup };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void engineStatus (const juce::String&) {}
        virtual void engineProgress (float /*0..1*/) {}
        virtual void engineProbed (const EngineInfo&) {}
        virtual void engineFinished (Task, bool /*ok*/, const juce::var& /*result*/,
                                     const juce::String& /*error*/) {}
    };

    EngineRunner();
    ~EngineRunner() override;

    // Solo desde el hilo de mensajes.
    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    bool startProbe();
    bool startSeparate (const juce::File& input, const juce::File& outDir, const SeparationOptions&);
    bool startSetup (bool cpuOnly);
    void cancel();

    bool isBusy() const { return isThreadRunning(); }
    Task currentTask() const { return task.load(); }
    bool hasProbed() const { return probed.load(); }

    EngineInfo getInfo() const;               // mejor motor encontrado (puede no ser valido)
    juce::StringArray getBaseCommand() const; // cualquier Python 3.10 - 3.12 (para "setup"); vacio si no hay

private:
    void run() override;
    void doProbe();
    void doSeparate();
    void doSetup();

    EngineInfo probeCommand (const juce::StringArray& command, const juce::File& script);
    int runStreaming (const juce::StringArray& args, juce::var& result, juce::String& error);
    void handleLine (const juce::String& line, juce::var& result, juce::String& error);
    static juce::File writeEngineScript();

    template <typename Fn>
    void post (Fn&& fn)
    {
        juce::WeakReference<EngineRunner> self (this);
        juce::MessageManager::callAsync ([self, fn = std::forward<Fn> (fn)]() mutable
        {
            if (self != nullptr)
                fn (*self);
        });
    }

    juce::ListenerList<Listener> listeners;

    std::atomic<Task> task { Task::none };
    std::atomic<bool> probed { false };
    std::atomic<bool> cancelled { false };

    // parametros del trabajo actual
    juce::File jobInput, jobOutDir;
    SeparationOptions jobOptions;
    bool jobCpuOnly = false;

    mutable juce::CriticalSection infoLock;
    EngineInfo bestInfo;
    juce::StringArray baseCommand;

    juce::CriticalSection procLock;
    juce::ChildProcess* current = nullptr;
    juce::StringArray logTail;

    JUCE_DECLARE_WEAK_REFERENCEABLE (EngineRunner)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineRunner)
};
